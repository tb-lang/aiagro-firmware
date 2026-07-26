/**
 * UNIUBE PIVOT WiFi p2 - SEMPRE CONECTADA (virada de tatica 25/jul, noite)
 *
 * p1 dormia 10min entre ciclos -> hotspot de celular desliga o AP ~90s sem
 * cliente, entao a placa podia acordar e nao achar a rede. p2 resolve na
 * raiz: ESP32 SEMPRE ligado e SEMPRE associado ao WiFi (cliente permanente
 * segura o hotspot no ar 24/7). Energia vem do pivo, consumo nao importa.
 * Mesma filosofia da receptora v11 "sempre acordada" validada em campo.
 *
 *  - Envia dados de 1 em 1 HORA (padrao pivo) + envio imediato no boot
 *  - WiFi cai -> reconecta sozinho em ate 30s (principal + fallback)
 *  - Reboot preventivo a cada 24h (limpa stack WiFi; RTC preserva ciclo/pluv)
 *  - Pluviometro conta por interrupcao continua (sem EXT0/deep sleep)
 *  - OTA checado a cada ciclo de envio (1x/hora)
 *
 * Pinagem AI Agro custom:
 *   RS485 TX 17 | RX 16 | DE/RE 32 | DHT22 4 | RELE 26 | VEXT 0
 *   PLUV 25 | VOLT 34 | OLED SDA 21 SCL 22
 *
 * Sensor 7x1 LOTE NOVO (ambos slaves) — regs 0x0000-0x0006 em 1 chamada.
 * ATENCAO: 0x0000 e 0x0001 TROCADOS vs datasheet (confirmado bancada 24/mai):
 *   0x0000 = TEMPERATURA (/10=C) | 0x0001 = UMIDADE (/10=%)
 *   0x0002 EC (uS/cm) | 0x0003 pH (/100) | 0x0004 N | 0x0005 P | 0x0006 K
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ModbusMaster.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ====== Versao do firmware (sincronizar com arquivo VERSION do repo) ======
#define VERSAO_FW "p2"

// ====== Config por dispositivo (defaults; sobrescritos por build_flags) ======
#ifndef DEVICE_CODIGO
  #define DEVICE_CODIGO "UNIUBE_WIFI_PIVOT"
#endif
#ifndef DISP_ID
  #define DISP_ID "19ac767a-1c2a-4257-a7fa-0afdea4b9b27"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID "AP 101"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS "Barbosan"
#endif
// Rede reserva: se a principal falhar, tenta esta. Vazio = sem fallback.
#ifndef WIFI_SSID_FB
  #define WIFI_SSID_FB ""
#endif
#ifndef WIFI_PASS_FB
  #define WIFI_PASS_FB ""
#endif

// ====== Endpoints fixos ======
const char* SUPABASE_URL = "https://bwtotmprzmldczafjhrg.supabase.co/rest/v1/leituras";
const char* SUPABASE_ANON_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJ3dG90bXByem1sZGN6YWZqaHJnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzgwNzI0MjAsImV4cCI6MjA5MzY0ODQyMH0.-ZiY9JSdsUoCC2dSsesYemH-vN61Gl5odX9XrRxc-jo";

// ====== URLs OTA ======
const char* OTA_URL_VERSION =
  "https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/pivot_uniube_wifi/VERSION";
const String OTA_URL_BINARIO =
  String("https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/pivot_uniube_wifi/builds/")
  + DEVICE_CODIGO + ".bin";

// ====== Ciclo ======
#define INTERVALO_ENVIO_MS   3600000UL   // 1 hora entre envios
#define MS_ENTRE_SLAVES      30000       // respiro RS485 entre slave 1 e slave 2
#define CHECK_WIFI_MS        30000       // checa/reconecta WiFi a cada 30s
#define REBOOT_PREVENTIVO_MS 86400000UL  // reboot a cada 24h (limpa stack WiFi)

// ====== Pinos ======
#define VEXT_PIN        0
#define OLED_SDA        21
#define OLED_SCL        22
#define OLED_RST        -1
#define DHTPIN          4
#define DHTTYPE         DHT22
#define RS485_TX        17
#define RS485_RX        16
#define RS485_DE_RE     32
#define RELE_PIN        26
#define VOLTIMETRO_PIN  34
#define PLUVIOMETRO_PIN 25

// ====== Globais ======
Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);
DHT dht(DHTPIN, DHTTYPE);
ModbusMaster node;
RTC_DATA_ATTR uint32_t pluviometroPulsos = 0;   // sobrevive ao reboot preventivo
RTC_DATA_ATTR uint32_t ciclo = 0;
volatile unsigned long ultimoPulsoMs = 0;
uint32_t pluviometroPulsosLidos = 0;
float temperaturaAr=0, umidadeAr=0;
float voltagemBateria = 0;
unsigned long ultimoEnvioMs = 0;
unsigned long ultimoCheckWifiMs = 0;
bool primeiroEnvioFeito = false;

struct LeituraSolo {
  bool ok;
  float umid_solo;
  float temp_solo;
  int   ec;
  float ph;
  int   n, p, k;
};

void IRAM_ATTR pluviometroISR() {
  unsigned long agora = millis();
  if (agora - ultimoPulsoMs > 250) { pluviometroPulsos++; ultimoPulsoMs = agora; }
}

void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW);  }

void mostrarStatus(String texto) {
  display.clearDisplay(); display.setCursor(0, 0);
  display.printf("%s V%s\n", DEVICE_CODIGO, VERSAO_FW);
  display.println("----------------");
  display.println(texto); display.display();
}

bool tentarRede(const char* ssid, const char* pass) {
  Serial.printf("Conectando WiFi [%s]", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK [%s] | IP: ", ssid); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.printf("WiFi FAIL [%s]\n", ssid);
  return false;
}

bool conectarWiFi(bool forcar = false) {
  if (forcar) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(300);
  } else if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  if (tentarRede(WIFI_SSID, WIFI_PASS)) return true;
  if (strlen(WIFI_SSID_FB) > 0) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(300);
    return tentarRede(WIFI_SSID_FB, WIFI_PASS_FB);
  }
  return false;
}

// ====== OTA ======
void verificarOTA() {
  if (!conectarWiFi()) { Serial.println("OTA: sem WiFi, pulando"); return; }
  Serial.printf("OTA: checando versao (atual: %s)...\n", VERSAO_FW);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String urlVersion = String(OTA_URL_VERSION) + "?cb=" + String(esp_random());
  if (!http.begin(client, urlVersion)) { Serial.println("OTA: falha abrir URL"); return; }
  int code = http.GET();
  if (code != 200) { Serial.printf("OTA: HTTP %d, pulando\n", code); http.end(); return; }
  String novaVersao = http.getString(); novaVersao.trim();
  http.end();
  if (novaVersao == VERSAO_FW) {
    Serial.printf("OTA: ja na versao mais recente (%s)\n", VERSAO_FW);
    return;
  }
  Serial.printf("OTA: nova versao disponivel: %s (atual %s)\n", novaVersao.c_str(), VERSAO_FW);
  String urlBin = OTA_URL_BINARIO + "?cb=" + String(esp_random());
  Serial.printf("OTA: baixando %s\n", urlBin.c_str());
  WiFiClientSecure clientUpdate; clientUpdate.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(clientUpdate, urlBin);
  if (ret == HTTP_UPDATE_FAILED)
    Serial.printf("OTA: FALHOU (%d): %s\n",
      httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
}

// Le um bloco de registradores com ate 5 retries internos + flush entre
// tentativas. Soluciona instabilidade do barramento RS485 sem terminacao
// (estrategia validada na Pivot V1 LoRa: slave 2 e intermitente sem isso).
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
  while (Serial2.available()) Serial2.read();
  Serial2.flush();
  delay(200);
  node.begin(slaveId, Serial2);
  delay(50);

  // LOTE NOVO: regs 0x0000-0x0006 em 1 chamada (temp/umid trocados)
  if (!tryRead(0x0000, 7)) {
    Serial.printf("  ERRO slave %d (apos 5 retries)\n", slaveId);
    return s;
  }
  s.temp_solo = node.getResponseBuffer(0) / 10.0;
  s.umid_solo = node.getResponseBuffer(1) / 10.0;
  s.ec        = node.getResponseBuffer(2);
  s.ph        = node.getResponseBuffer(3) / 100.0;
  s.n         = node.getResponseBuffer(4);
  s.p         = node.getResponseBuffer(5);
  s.k         = node.getResponseBuffer(6);
  s.ok = true;
  Serial.printf("  s%d OK: umid=%.1f%% temp=%.1fC EC=%d pH=%.2f N=%d P=%d K=%d\n",
                slaveId, s.umid_solo, s.temp_solo, s.ec, s.ph, s.n, s.p, s.k);
  return s;
}

void lerAr() {
  analogSetAttenuation(ADC_11db);
  voltagemBateria = analogRead(VOLTIMETRO_PIN);
  temperaturaAr   = dht.readTemperature();
  umidadeAr       = dht.readHumidity();
  if (isnan(temperaturaAr)) temperaturaAr = 0;
  if (isnan(umidadeAr))     umidadeAr     = 0;
  noInterrupts(); pluviometroPulsosLidos = pluviometroPulsos; interrupts();
  Serial.printf("[AR] %.1fC/%.1f%% bat=%.0f pluv=%u\n",
    temperaturaAr, umidadeAr, voltagemBateria, pluviometroPulsosLidos);
}

bool postParaSupabase(const LeituraSolo& s, int sensorPos, int pacote) {
  StaticJsonDocument<512> doc;
  doc["dispositivo_id"]      = DISP_ID;
  doc["versao_fw"]           = VERSAO_FW;
  doc["ciclo"]               = ciclo;
  doc["pacote"]              = pacote;
  doc["sensor_pos"]          = sensorPos;
  doc["umid_solo"]           = s.umid_solo;
  doc["temp_solo"]           = s.temp_solo;
  doc["ec"]                  = s.ec;
  doc["ph"]                  = s.ph;
  doc["n_mg_kg"]             = s.n;
  doc["p_mg_kg"]             = s.p;
  doc["k_mg_kg"]             = s.k;
  doc["temp_ar"]             = temperaturaAr;
  doc["umid_ar"]             = umidadeAr;
  doc["pluviometro_pulsos"]  = pluviometroPulsosLidos;
  doc["voltagem_bateria"]    = (int)voltagemBateria;
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  int sinal = rssi >= -30 ? 100 : (rssi <= -100 ? 0 : map(rssi, -100, -30, 0, 100));
  doc["sinal_wifi_pct"]      = sinal;

  String jsonStr; serializeJson(doc, jsonStr);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, SUPABASE_URL)) return false;
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(10000);
  int code = http.POST(jsonStr);
  Serial.printf("[Supabase] s%d HTTP %d\n", sensorPos, code);
  if (code != 201 && code > 0) {
    String resp = http.getString();
    if (resp.length() < 300) Serial.println(resp);
  }
  http.end();
  return (code == 201);
}

void cicloDeEnvio() {
  ciclo++;
  Serial.printf("\n=== %s V%s | Ciclo %u | uptime %lumin ===\n",
    DEVICE_CODIGO, VERSAO_FW, ciclo, millis() / 60000UL);

  digitalWrite(RELE_PIN, LOW);   // liga os 2 sensores 7x1
  delay(3000);                   // aquecimento

  // Leitura de aquecimento (descartada) — primeira leitura vem com lixo
  Serial.println("--- Leitura de aquecimento (descartada) ---");
  lerSensorSolo(1);
  delay(500);
  lerSensorSolo(2);
  delay(2000);
  Serial.println("--- Sensores aquecidos, ciclo real ---");

  // SLAVE 1: le e envia
  lerAr();
  Serial.println("> SLAVE 1");
  LeituraSolo s1 = lerSensorSolo(1);
  if (conectarWiFi()) {
    mostrarStatus("ENVIANDO s1");
    postParaSupabase(s1, 1, 1);
  }

  // Respiro do barramento RS485 antes do slave 2
  Serial.printf("> respiro %dms antes do slave 2\n", MS_ENTRE_SLAVES);
  delay(MS_ENTRE_SLAVES);

  // SLAVE 2: le e envia
  Serial.println("> SLAVE 2");
  LeituraSolo s2 = lerSensorSolo(2);
  if (conectarWiFi()) {
    mostrarStatus("ENVIANDO s2");
    postParaSupabase(s2, 2, 2);
  }

  digitalWrite(RELE_PIN, HIGH);   // desliga sensores ate o proximo ciclo

  mostrarStatus("CHECANDO OTA");
  verificarOTA();

  mostrarStatus("OK ciclo " + String(ciclo) + "\nprox em 60min\nWiFi " +
                String(WiFi.status() == WL_CONNECTED ? "conectado" : "CAIU"));
  Serial.println("Ciclo completo. Proximo em 60min (placa segue acordada).");
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW); delay(1000);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, HIGH);   // rele desligado
  pinMode(RS485_DE_RE, OUTPUT); postTransmission();
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextColor(WHITE); display.setTextSize(1);
  dht.begin();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  pinMode(PLUVIOMETRO_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PLUVIOMETRO_PIN), pluviometroISR, FALLING);

  Serial.printf("\n=== %s V%s | boot (sempre conectada) ===\n", DEVICE_CODIGO, VERSAO_FW);
  mostrarStatus("CONECTANDO...");
  conectarWiFi();
  // Primeiro envio sai imediato no loop() (primeiroEnvioFeito=false)
}

void loop() {
  unsigned long agora = millis();

  // Reboot preventivo diario (RTC preserva ciclo e pluviometro)
  if (agora > REBOOT_PREVENTIVO_MS) {
    Serial.println("Reboot preventivo 24h...");
    ESP.restart();
  }

  // Vigia do WiFi: mantem a placa SEMPRE associada (segura o hotspot no ar)
  if (agora - ultimoCheckWifiMs >= CHECK_WIFI_MS) {
    ultimoCheckWifiMs = agora;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] caiu — reconectando...");
      conectarWiFi(true);
    }
  }

  // Envio: imediato no boot, depois de 1 em 1 hora
  if (!primeiroEnvioFeito || (agora - ultimoEnvioMs >= INTERVALO_ENVIO_MS)) {
    cicloDeEnvio();
    ultimoEnvioMs = millis();
    primeiroEnvioFeito = true;
  }

  delay(250);
}
