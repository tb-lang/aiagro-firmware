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

// ====== Registradores 7x1 ======
#define REG_UMID  0x0000
#define REG_TEMP  0x0001
#define REG_EC    0x0002
#define REG_PH    0x0003
#define REG_N     0x0004
#define REG_P     0x0005
#define REG_K     0x0006

// ====== Ciclo ======
const uint8_t NUM_ENVIOS         = 3;
const uint32_t MS_ENTRE_SENSORES = 30000;
const uint32_t MS_ENTRE_ENVIOS   = 60000;
const uint64_t SLEEP_SEGUNDOS    = 3600;    // 1 hora entre ciclos

// ====== Estado persistente entre boots ======
RTC_DATA_ATTR uint32_t ciclo = 0;

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

void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW); }

LeituraSolo lerSensorSolo(uint8_t slaveId) {
  LeituraSolo s = {false, 0, 0, 0, 0, 0, 0, 0};
  node.begin(slaveId, Serial2);
  delay(50);
  uint8_t r = node.readHoldingRegisters(REG_UMID, 7);
  if (r != node.ku8MBSuccess) {
    Serial.printf("  ERRO leitura slave %d (codigo %d)\n", slaveId, r);
    return s;
  }
  s.temp_solo = node.getResponseBuffer(0) / 10.0;   // reg[0] = TEMPERATURA (estava trocado)
  s.umid_solo = node.getResponseBuffer(1) / 10.0;   // reg[1] = UMIDADE (estava trocado)
  s.ec        = node.getResponseBuffer(2);
  s.ph        = node.getResponseBuffer(3) / 100.0;
  s.n         = node.getResponseBuffer(4);
  s.p         = node.getResponseBuffer(5);
  s.k         = node.getResponseBuffer(6);
  s.ok = true;
  Serial.printf("  s%d OK: umid=%.1f%% temp=%.1fC EC=%u pH=%.2f N=%u P=%u K=%u\n",
                slaveId, s.umid_solo, s.temp_solo, s.ec, s.ph, s.n, s.p, s.k);
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
  String jsonStr;
  serializeJson(doc, jsonStr);
  Serial.printf("TX pacote %u (s%d) | %d bytes\n", pacote, sensorId, jsonStr.length());
  Serial.println(jsonStr);
  LoRa.beginPacket();
  LoRa.print(jsonStr);
  LoRa.endPacket();
}

void dormir() {
  Serial.printf("Dormindo %llus...\n", SLEEP_SEGUNDOS);
  digitalWrite(RELE_PIN, HIGH);
  digitalWrite(VEXT_PIN, HIGH);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SLEEP_SEGUNDOS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  ciclo++;
  Serial.printf("\n=========================================\n");
  Serial.printf("%s V%s | Ciclo %u\n", ORIGEM_ESTACAO, VERSAO_FW, ciclo);
  Serial.printf("=========================================\n");

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, LOW);
  pinMode(RS485_DE_RE, OUTPUT); postTransmission();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  dht.begin();

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

  uint16_t pacote = 0;
  for (uint8_t env = 0; env < NUM_ENVIOS; env++) {
    Serial.printf("\n--- Envio %d/%d ---\n", env + 1, NUM_ENVIOS);
    LeituraAr a = lerAr();
    LeituraSolo s1 = lerSensorSolo(1);
    if (s1.ok) { pacote++; enviarPacote(1, s1, a, pacote); }
    delay(MS_ENTRE_SENSORES);
    LeituraSolo s2 = lerSensorSolo(2);
    if (s2.ok) { pacote++; enviarPacote(2, s2, a, pacote); }
    if (env < NUM_ENVIOS - 1) {
      Serial.printf("Aguardando %lums ate proximo envio...\n",
                    MS_ENTRE_ENVIOS - MS_ENTRE_SENSORES);
      delay(MS_ENTRE_ENVIOS - MS_ENTRE_SENSORES);
    }
  }

  Serial.printf("\nCiclo %u completo (%u pacotes enviados).\n", ciclo, pacote);
  dormir();
}

void loop() {}
