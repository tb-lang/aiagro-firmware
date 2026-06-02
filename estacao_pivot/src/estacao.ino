/**
 * ESTACAO PIVOT - AI AGRO ESP32 + RA-02 (GENERICA)
 *
 * 1 source serve TODAS as estacoes de pivo. O que muda por dispositivo
 * vem via build_flags no platformio.ini:
 *   -DORIGEM_ESTACAO   ex: "PIVOT_EST1" (Cristalina) ou "UNIUBE_PIVOT_EST1"
 *
 * Le 2x sensor 7x1 RS485 (slave 1 + slave 2) + DHT22 (ar).
 * Manda 6 pacotes LoRa por ciclo (3 envios x 2 sensores).
 * Deep sleep 1h entre ciclos.
 *
 * SEM OTA: estacao de pivo fica longe do roteador, sem WiFi. Atualizacao
 * so por flash fisico. (Por isso nao tem VERSAO checada via GitHub.)
 *
 * REGISTRADORES 7x1 (lote novo) — ATENCAO: 0x0000 e 0x0001 estao
 * TROCADOS em relacao ao datasheet. Confirmado em bancada (24/mai):
 *   0x0000 = TEMPERATURA (/10=C)  | 0x0001 = UMIDADE (/10=%)
 *   0x0002 EC (uS/cm) | 0x0003 pH (/100) | 0x0004 N | 0x0005 P | 0x0006 K (mg/kg)
 *   (validado: na agua reg[1]=1000=100%; terra seca->molhada reg[1] sobe)
 *
 * PINOS (Placa AI Agro): LoRa SCK 18|MISO 19|MOSI 23|SS 5|RST 14|DIO0 27
 *                        RS485 TX 17|RX 16|DE/RE 32 | VEXT 0|RELE 26|DHT22 4
 */
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <ModbusMaster.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"   // desabilita brown-out detector

// ====== Versao (so referencia; estacao pivot nao tem OTA) ======
#define VERSAO_FW "1"

// ====== Config por dispositivo (default; sobrescrito por build_flags) ======
#ifndef ORIGEM_ESTACAO
  #define ORIGEM_ESTACAO "PIVOT_EST1"
#endif

// ====== Pinos ======
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23
#define LORA_SS      5
#define LORA_RST    14
#define LORA_DIO0   27
#define RS485_TX    17
#define RS485_RX    16
#define RS485_DE_RE 32
#define RELE_PIN    26
#define VEXT_PIN     0
#define DHTPIN       4
#define DHTTYPE     DHT22
#define PLUV_PIN    25   // pluviometro Hall A3144 (INPUT_PULLUP, FALLING)
#define VOLT_PIN    34   // divisor de tensao da bateria (ADC1_CH6)

// ====== Registradores 7x1 ======
#define REG_UMID  0x0000
#define REG_TEMP  0x0001
#define REG_EC    0x0002
#define REG_PH    0x0003
#define REG_N     0x0004
#define REG_P     0x0005
#define REG_K     0x0006

// ====== Ciclo ======
// Override via build_flags (env de teste). Default = producao (3 envios, 1h).
#ifndef N_ENVIOS
#define N_ENVIOS 3
#endif
#ifndef SLEEP_SEG
#define SLEEP_SEG 3600          // 1 hora entre ciclos (producao)
#endif
#ifndef FORCAR_ENVIO
#define FORCAR_ENVIO 0          // 1 = envia mesmo se sensor falhar (modo teste de cadeia)
#endif
#ifndef MONITOR_PLUV_SEG
#define MONITOR_PLUV_SEG 0      // >0 = abre janela de monitoramento do pluviometro
                                // apos os envios, imprime cada virada e envia
                                // 1 pacote extra com o total ao final
#endif
const uint8_t NUM_ENVIOS         = N_ENVIOS;
const uint32_t MS_ENTRE_SENSORES = 30000;
const uint32_t MS_ENTRE_ENVIOS   = 60000;
const uint64_t SLEEP_SEGUNDOS    = SLEEP_SEG;

// ====== Estado persistente entre boots ======
RTC_DATA_ATTR uint32_t ciclo       = 0;
RTC_DATA_ATTR uint32_t pluv_total  = 0;   // acumula viradas entre ciclos

// ====== Globais ======
ModbusMaster node;
DHT dht(DHTPIN, DHTTYPE);

struct LeituraSolo {
  bool ok;
  float umid_solo;
  float temp_solo;
  uint16_t ec;
  float ph;
  uint16_t n, p, k;
};

struct LeituraAr {
  bool ok;
  float temp_ar;
  float umid_ar;
};

// Le voltagem da bateria via divisor no GPIO 34 (ADC1_CH6).
// Retorna valor RAW do ADC (0-4095). Conversao pra Volts depende do divisor
// usado em cada placa — calibra no Supabase/dashboard.
// Estacoes alimentadas por rede eletrica (sem bateria) compilar com
// -DSEM_BATERIA=1: retorna 0 e nao gasta tempo lendo o ADC.
uint16_t lerBateriaRaw() {
#ifdef SEM_BATERIA
  return 0;
#else
  analogSetAttenuation(ADC_11db);
  // 4 leituras pra fazer media simples e suavizar ruido
  uint32_t soma = 0;
  for (int i = 0; i < 4; i++) { soma += analogRead(VOLT_PIN); delay(5); }
  return soma / 4;
#endif
}

// ====== Pluviometro (interrupcao FALLING + debounce 30ms) ======
// (definido apos as structs pra nao confundir o auto-prototype do .ino,
//  que insere prototypes na linha da primeira funcao do arquivo)
volatile uint32_t pluv_pulsos = 0;
volatile uint32_t pluv_lastMs = 0;
void IRAM_ATTR onPluv() {
  uint32_t now = millis();
  if (now - pluv_lastMs < 30) return;
  pluv_lastMs = now;
  pluv_pulsos++;
}

void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW); }

// Decide, slave-a-slave, se le no mapa LOTE VELHO ou LOTE NOVO.
//  - Default: ambos lote NOVO (Cristalina).
//  - Build flag LOTE_VELHO=1 => ambos lote VELHO (Cafe/Bela puros).
//  - Build flag S1_LOTE_VELHO=1 => so slave 1 e velho (mix: Uniube Pivot).
//  - Build flag S2_LOTE_VELHO=1 => so slave 2 e velho.
#ifndef S1_LOTE_VELHO
  #ifdef LOTE_VELHO
    #define S1_LOTE_VELHO 1
  #else
    #define S1_LOTE_VELHO 0
  #endif
#endif
#ifndef S2_LOTE_VELHO
  #ifdef LOTE_VELHO
    #define S2_LOTE_VELHO 1
  #else
    #define S2_LOTE_VELHO 0
  #endif
#endif

// Le um bloco de registradores com ate 5 retries internos + flush entre
// tentativas. Soluciona instabilidade do barramento RS485 sem terminacao.
bool tryRead(uint16_t reg, uint16_t qtd) {
  for (uint8_t t = 1; t <= 5; t++) {
    while (Serial2.available()) Serial2.read();
    delay(150);
    uint8_t r = node.readHoldingRegisters(reg, qtd);
    if (r == node.ku8MBSuccess) return true;
    delay(300);
  }
  return false;
}

LeituraSolo lerSensorSolo(uint8_t slaveId) {
  LeituraSolo s = {false, 0, 0, 0, 0, 0, 0, 0};
  // Limpa buffer + espera barramento RS485 silenciar antes de trocar de slave
  while (Serial2.available()) Serial2.read();
  Serial2.flush();
  delay(200);
  node.begin(slaveId, Serial2);
  delay(50);

  bool ehLoteVelho = (slaveId == 1) ? S1_LOTE_VELHO : S2_LOTE_VELHO;

  if (ehLoteVelho) {
    // LOTE VELHO: regs 0x0012(umid /10) 0x0013(temp /10) 0x0015(EC) 0x0006(pH /100) 0x001E(NPK)
    bool okUT  = tryRead(0x0012, 2);
    if (okUT) {
      s.umid_solo = node.getResponseBuffer(0) / 10.0;
      s.temp_solo = node.getResponseBuffer(1) / 10.0;
    }
    if (tryRead(0x0015, 1)) s.ec = node.getResponseBuffer(0);
    if (tryRead(0x0006, 1)) s.ph = node.getResponseBuffer(0) / 100.0;
    if (tryRead(0x001E, 3)) {
      s.n = node.getResponseBuffer(0); s.p = node.getResponseBuffer(1); s.k = node.getResponseBuffer(2);
    }
    if (!okUT) {
      Serial.printf("  ERRO slave %d LOTE VELHO (umid+temp falharam apos 5 retries)\n", slaveId);
      return s;
    }
  } else {
    // LOTE NOVO (Cristalina): regs 0x0000-0x0006 sequenciais (umid/temp trocados)
    if (!tryRead(REG_UMID, 7)) {
      Serial.printf("  ERRO slave %d LOTE NOVO (apos 5 retries)\n", slaveId);
      return s;
    }
    s.temp_solo = node.getResponseBuffer(0) / 10.0;
    s.umid_solo = node.getResponseBuffer(1) / 10.0;
    s.ec        = node.getResponseBuffer(2);
    s.ph        = node.getResponseBuffer(3) / 100.0;
    s.n         = node.getResponseBuffer(4);
    s.p         = node.getResponseBuffer(5);
    s.k         = node.getResponseBuffer(6);
  }
  s.ok = true;
  Serial.printf("  s%d (%s) OK: umid=%.1f%% temp=%.1fC EC=%u pH=%.2f N=%u P=%u K=%u\n",
                slaveId, ehLoteVelho?"VELHO":"NOVO ",
                s.umid_solo, s.temp_solo, s.ec, s.ph, s.n, s.p, s.k);
  return s;
}

LeituraAr lerAr() {
  LeituraAr a = {false, 0, 0};
  a.temp_ar = dht.readTemperature();
  a.umid_ar = dht.readHumidity();
  if (isnan(a.temp_ar) || isnan(a.umid_ar)) {
    Serial.println("  ERRO leitura DHT22");
    a.temp_ar = 0; a.umid_ar = 0;
    return a;
  }
  a.ok = true;
  Serial.printf("  AR OK: temp=%.1fC umid=%.1f%%\n", a.temp_ar, a.umid_ar);
  return a;
}

bool iniciarLoRa() {
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW); delay(100);
  digitalWrite(LORA_RST, HIGH); delay(200);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  for (int i = 1; i <= 5; i++) {
    if (LoRa.begin(915E6)) {
      LoRa.setSpreadingFactor(7);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setCodingRate4(5);
      Serial.println("LoRa OK (915MHz / SF7 / BW125 / CR4/5)");
      return true;
    }
    Serial.printf("LoRa tentativa %d falhou\n", i);
    delay(500);
  }
  return false;
}

void enviarPacote(uint8_t sensorId, const LeituraSolo& s, const LeituraAr& a, uint16_t pacote) {
  StaticJsonDocument<384> doc;
  doc["origem"] = ORIGEM_ESTACAO;
  doc["versao"] = VERSAO_FW;
  doc["ciclo"]  = ciclo;
  doc["pacote"] = pacote;
  doc["sensor"] = sensorId;
  doc["umid_solo"] = s.umid_solo;
  doc["temp_solo"] = s.temp_solo;
  doc["ec"]        = s.ec;
  doc["ph"]        = s.ph;
  doc["n"]         = s.n;
  doc["p"]         = s.p;
  doc["k"]         = s.k;
  doc["temp_ar"]   = a.temp_ar;
  doc["umid_ar"]   = a.umid_ar;
  doc["pluv"]      = pluv_total + pluv_pulsos;  // viradas acumuladas
  doc["bat"]       = lerBateriaRaw();           // ADC raw 0-4095
  String jsonStr;
  serializeJson(doc, jsonStr);
  Serial.printf("TX pacote %u (s%d) | %d bytes\n", pacote, sensorId, jsonStr.length());
  Serial.println(jsonStr);
  LoRa.beginPacket();
  LoRa.print(jsonStr);
  LoRa.endPacket();
}

// Dorme N segundos com duplo wake:
//  - timer (acordar pra ciclo de envio)
//  - EXT0 no GPIO 25 LOW (cada virada do pluviometro acorda, conta, volta a dormir)
void dormirSegundos(uint64_t segundos) {
  Serial.printf("Dormindo %llus...\n", segundos);
  digitalWrite(RELE_PIN, HIGH);
  digitalWrite(VEXT_PIN, HIGH);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(segundos * 1000000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUV_PIN, 0);  // LOW = virada
  esp_deep_sleep_start();
}

void dormir() { dormirSegundos(SLEEP_SEGUNDOS); }

// Espera o ima passar (GPIO voltar a HIGH) antes de dormir de novo —
// senao o EXT0 (level-triggered) re-acorda em loop infinito.
void esperarImaPassar() {
  uint32_t t0 = millis();
  while (digitalRead(PLUV_PIN) == LOW && (millis() - t0) < 5000) delay(5);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // desabilita brown-out detector
                                              // (evita reset loop quando alimentacao
                                              //  fica no limite — fonte fraca, bateria
                                              //  baixa, regulador puxando picos)
  Serial.begin(115200);
  delay(500);

  // === Detecta causa do wakeup ANTES de tudo ===
  esp_sleep_wakeup_cause_t causa = esp_sleep_get_wakeup_cause();

  // Pino do pluv tem que estar pronto ja na chegada
  pinMode(PLUV_PIN, INPUT_PULLUP);

  // Se acordou por VIRADA DO BALDE (EXT0), so conta e volta a dormir.
  // Nao liga rele, nao le sensor, nao mexe em LoRa — economiza bateria.
  if (causa == ESP_SLEEP_WAKEUP_EXT0) {
    pluv_total++;
    Serial.printf("[PLUV WAKE] virada #%u — voltando a dormir\n", pluv_total);
    esperarImaPassar();   // espera GPIO voltar HIGH (evita re-acordar em loop)
    // Dorme o restante ate completar o ciclo. Como nao temos relogio absoluto
    // aqui, dorme um chute pequeno (60s) — proxima virada/timer corta de novo.
    // (Em producao com RTC real -> guardar deadline em RTC_DATA_ATTR.)
    dormirSegundos(SLEEP_SEGUNDOS);  // re-arma timer + EXT0
  }

  ciclo++;
  Serial.printf("\n=========================================\n");
  Serial.printf("%s V%s | Ciclo %u (wake=%d)\n",
                ORIGEM_ESTACAO, VERSAO_FW, ciclo, (int)causa);
  Serial.printf("=========================================\n");

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, LOW);
  pinMode(RS485_DE_RE, OUTPUT); postTransmission();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  dht.begin();

  // Tambem atacha interrupt em-execucao (conta viradas que rolam DURANTE o ciclo)
  attachInterrupt(digitalPinToInterrupt(PLUV_PIN), onPluv, FALLING);
  Serial.printf("Pluv GPIO%d=%s  (total acumulado=%u)\n",
                PLUV_PIN, digitalRead(PLUV_PIN)?"HIGH":"LOW", pluv_total);

  Serial.println("Aguardando sensores estabilizarem (3s)...");
  delay(3000);

  if (!iniciarLoRa()) {
    Serial.println("LoRa FALHOU. Dormindo direto.");
    dormir();
  }

  // Leitura de aquecimento (descartada) - primeira leitura vem com lixo
  Serial.println("\n--- Leitura de aquecimento (descartada) ---");
  lerSensorSolo(1);
  delay(500);
  lerSensorSolo(2);
  delay(2000);
  Serial.println("--- Sensores aquecidos, comecando ciclo real ---");

  // ===== Le e envia ALTERNADAMENTE: 1 sensor por vez, com respiro grande
  //       entre slaves. Evita colisao no barramento RS485 sem terminacao.
  uint16_t pacote = 0;
  for (uint8_t env = 0; env < NUM_ENVIOS; env++) {
    Serial.printf("\n--- Envio %d/%d ---\n", env + 1, NUM_ENVIOS);
    LeituraAr a = lerAr();

    // --- SLAVE 1 (cada chamada Modbus interna ja tem 5 retries) ---
    Serial.println("  > lendo SLAVE 1");
    LeituraSolo s1 = lerSensorSolo(1);
    if (s1.ok || FORCAR_ENVIO) { pacote++; enviarPacote(1, s1, a, pacote); }

    Serial.printf("  > respiro %lums antes do slave 2\n", MS_ENTRE_SENSORES);
    delay(MS_ENTRE_SENSORES);

    // --- SLAVE 2 ---
    Serial.println("  > lendo SLAVE 2");
    LeituraSolo s2 = lerSensorSolo(2);
    if (s2.ok || FORCAR_ENVIO) { pacote++; enviarPacote(2, s2, a, pacote); }

    if (env < NUM_ENVIOS - 1) {
      Serial.printf("  > respiro %lums ate proximo envio\n", MS_ENTRE_SENSORES);
      delay(MS_ENTRE_SENSORES);
    }
  }

  Serial.printf("\nCiclo %u completo (%u pacotes enviados).\n", ciclo, pacote);

  // ====== Janela de monitoramento do pluviometro (modo teste) ======
  if (MONITOR_PLUV_SEG > 0) {
    Serial.printf("\n=== MONITOR PLUV: %d segundos — vira o balde! ===\n",
                  (int)MONITOR_PLUV_SEG);
    Serial.printf("GPIO%d estado=%s  pulsos ate agora=%u\n",
                  PLUV_PIN, digitalRead(PLUV_PIN)?"HIGH":"LOW", pluv_pulsos);
    uint32_t t0 = millis();
    uint32_t ultimo = 0;
    int estado_ant = digitalRead(PLUV_PIN);
    while (millis() - t0 < (uint32_t)MONITOR_PLUV_SEG * 1000UL) {
      int estado = digitalRead(PLUV_PIN);
      if (estado != estado_ant) {
        Serial.printf("[POLL] GPIO%d %s -> %s   (interrupcoes=%u)\n",
                      PLUV_PIN,
                      estado_ant?"HIGH":"LOW ",
                      estado?"HIGH":"LOW",
                      pluv_pulsos);
        estado_ant = estado;
      }
      if (pluv_pulsos != ultimo) {
        Serial.printf(">>> PLUV VIROU! total=%u (acumulado %u)\n",
                      pluv_pulsos, pluv_total + pluv_pulsos);
        ultimo = pluv_pulsos;
      }
      if ((millis() - t0) % 5000 < 10) {
        Serial.printf("[heartbeat] t=%lus  GPIO=%s  pulsos=%u\n",
                      (millis()-t0)/1000,
                      estado?"HIGH":"LOW", pluv_pulsos);
        delay(15);
      }
      delay(5);
    }
    // pacote final com pluv atualizado
    Serial.printf("\n=== FIM MONITOR: %u viradas detectadas neste ciclo ===\n",
                  pluv_pulsos);
    LeituraAr a = lerAr();
    LeituraSolo s = lerSensorSolo(1);
    pacote++;
    enviarPacote(99, s, a, pacote);   // sensor=99 marca pacote de pluv final
  }

  // acumula viradas no RTC pro proximo ciclo nao perder
  pluv_total += pluv_pulsos;
  Serial.printf("Pluv acumulado salvo: %u\n", pluv_total);

  dormir();
}

void loop() {}
