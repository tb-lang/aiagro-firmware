/**
 * ESTACAO WIFI - AI AGRO ESP32 (GENERICA + OTA)
 *
 * Estacao com WiFi direto (sem LoRa, sem receptora). Le sensores e faz
 * POST direto pro Supabase + Google Sheets (dual write na transicao).
 *
 * Config por dispositivo via build_flags no platformio.ini:
 *   -DDEVICE_CODIGO   ex: "BELA_WIFI_EST1"
 *   -DORIGEM          ex: "BELA_WIFI_EST1" (campo origem no JSON)
 *   -DDISP_ID         UUID do dispositivo no Supabase
 *   -DWIFI_SSID / -DWIFI_PASS
 *
 * Hardware: 1x sensor 7x1 RS485 (LOTE ANTIGO: regs 0x0012/0x0015/0x0007/0x001E)
 *           + DHT22 + pluviometro (reed) + voltimetro de bateria. SEM OLED.
 *
 * OTA: tem WiFi, entao checa GitHub no boot. (estacao_wifi/VERSION + builds/)
 *
 * Ciclo: acorda (timer 1h OU pulso de pluviometro) -> 3 envios -> OTA -> dorme.
 *
 * PINOS: RS485 TX 17|RX 16|DE/RE 32 | DHT22 4 | RELE 26 | VEXT 0
 *        VOLTIMETRO 34 | PLUVIOMETRO 25
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ModbusMaster.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ====== Versao (sincronizar com arquivo VERSION do repo) ======
#define VERSAO_FW "1"

// ====== Config por dispositivo (default; sobrescrito por build_flags) ======
#ifndef DEVICE_CODIGO
  #define DEVICE_CODIGO "BELA_WIFI_EST1"
#endif
#ifndef ORIGEM
  #define ORIGEM "BELA_WIFI_EST1"
#endif
#ifndef DISP_ID
  #define DISP_ID "2f33249c-4c20-4f47-92c5-3989b50408d8"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID "Ap6401-2G"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS "21012003"
#endif

// ====== Pinos ======
#define RS485_TX        17
#define RS485_RX        16
#define RS485_DE_RE     32
#define DHTPIN          4
#define DHTTYPE         DHT22
#define RELE_PIN        26
#define VEXT_PIN        0
#define VOLTIMETRO_PIN  34
#define PLUVIOMETRO_PIN 25

// ====== Ciclo ======
#define NUM_ENVIOS         3
#define MS_ENTRE_ENVIOS    60000        // 1 min entre envios
const uint64_t SLEEP_SEGUNDOS = 3600;   // 1 hora entre ciclos (TESTE)

// ====== Endpoints fixos ======
const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/macros/s/AKfycby9tF9b7n79DXCQ8qT47U_0Ao-9eiUhsNe6iDo8579LLQOscyLphv2C4UxR7Y6Orn3P/exec";
const char* SUPABASE_URL = "https://bwtotmprzmldczafjhrg.supabase.co/rest/v1/leituras";
const char* SUPABASE_ANON_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJ3dG90bXByem1sZGN6YWZqaHJnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzgwNzI0MjAsImV4cCI6MjA5MzY0ODQyMH0.-ZiY9JSdsUoCC2dSsesYemH-vN61Gl5odX9XrRxc-jo";

// ====== OTA ======
const char* OTA_URL_VERSION =
  "https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/estacao_wifi/VERSION";
const String OTA_URL_BINARIO =
  String("https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/estacao_wifi/builds/")
  + DEVICE_CODIGO + ".bin";

// ====== Globais ======
ModbusMaster node;
DHT dht(DHTPIN, DHTTYPE);

RTC_DATA_ATTR uint32_t ciclo = 0;
RTC_DATA_ATTR uint32_t pluviometroPulsos = 0;
volatile unsigned long ultimoPulsoMs = 0;

float temperaturaAr = 0, umidadeAr = 0;
float umidadeSolo = 0, tempSolo = 0, phSolo = 0, condutividade = 0;
int nitrogenio = 0, fosforo = 0, potassio = 0;
float voltagemBateria = 0;
uint32_t pluviometroLido = 0;

void IRAM_ATTR pluviometroISR() {
  unsigned long agora = millis();
  if (agora - ultimoPulsoMs > 250) { pluviometroPulsos++; ultimoPulsoMs = agora; }
}

void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW); }

bool conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("Conectando WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK | IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("WiFi FAIL");
  return false;
}

// ====== OTA ======
void verificarOTA() {
  if (!conectarWiFi()) { Serial.println("OTA: sem WiFi, pulando"); return; }
  Serial.printf("OTA: checando versao (atual: %s)...\n", VERSAO_FW);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String urlVersion = String(OTA_URL_VERSION) + "?cb=" + String(esp_random());
  if (!http.begin(client, urlVersion)) { Serial.println("OTA: falha abrir URL"); return; }
  int code = http.GET();
  if (code != 200) { Serial.printf("OTA: HTTP %d, pulando\n", code); http.end(); return; }
  String novaVersao = http.getString();
  novaVersao.trim();
  http.end();
  if (novaVersao == VERSAO_FW) {
    Serial.printf("OTA: ja na versao mais recente (%s)\n", VERSAO_FW);
    return;
  }
  Serial.printf("OTA: nova versao %s (atual %s)\n", novaVersao.c_str(), VERSAO_FW);
  String urlBin = OTA_URL_BINARIO + "?cb=" + String(esp_random());
  Serial.printf("OTA: baixando %s\n", urlBin.c_str());
  WiFiClientSecure clientUpdate;
  clientUpdate.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(clientUpdate, urlBin);
  if (ret == HTTP_UPDATE_FAILED) {
    Serial.printf("OTA: FALHOU (%d): %s\n",
      httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  }
}

// ====== Leitura sensores (LOTE ANTIGO - registradores da Fazenda V44) ======
void lerSensores() {
  analogSetAttenuation(ADC_11db);
  voltagemBateria = analogRead(VOLTIMETRO_PIN);
  temperaturaAr   = dht.readTemperature();
  umidadeAr       = dht.readHumidity();
  if (isnan(temperaturaAr)) temperaturaAr = 0;
  if (isnan(umidadeAr))     umidadeAr = 0;

  if (node.readHoldingRegisters(0x0012, 2) == node.ku8MBSuccess) {
    umidadeSolo = node.getResponseBuffer(0);          // lote antigo: raw
    tempSolo    = node.getResponseBuffer(1) / 10.0;
  }
  if (node.readHoldingRegisters(0x0015, 1) == node.ku8MBSuccess)
    condutividade = node.getResponseBuffer(0);
  if (node.readHoldingRegisters(0x0007, 1) == node.ku8MBSuccess)
    phSolo = node.getResponseBuffer(0);               // lote antigo: raw
  if (node.readHoldingRegisters(0x001E, 3) == node.ku8MBSuccess) {
    nitrogenio = node.getResponseBuffer(0);
    fosforo    = node.getResponseBuffer(1);
    potassio   = node.getResponseBuffer(2);
  }
  noInterrupts(); pluviometroLido = pluviometroPulsos; interrupts();

  Serial.printf("  AR: temp=%.1fC umid=%.1f%%\n", temperaturaAr, umidadeAr);
  Serial.printf("  SOLO: umid=%.0f temp=%.1fC EC=%.0f pH=%.0f N=%d P=%d K=%d\n",
                umidadeSolo, tempSolo, condutividade, phSolo, nitrogenio, fosforo, potassio);
  Serial.printf("  BAT: %.0f | PLUV: %u viradas\n", voltagemBateria, pluviometroLido);
}

int rssiParaPct(int rssi) {
  if (rssi >= -50) return 100;
  if (rssi <= -100) return 0;
  return 2 * (rssi + 100);
}

bool postParaSheets(int indice) {
  StaticJsonDocument<512> doc;
  doc["origem"]     = ORIGEM;
  doc["versao"]     = VERSAO_FW;
  doc["ciclo"]      = ciclo;
  doc["pacote"]     = indice + 1;
  doc["sinal_wifi"] = rssiParaPct(WiFi.RSSI());
  JsonArray arr = doc.createNestedArray("sensores");
  JsonObject s = arr.createNestedObject();
  s["id"]        = 1;
  s["umid_solo"] = umidadeSolo;
  s["temp_solo"] = tempSolo;
  s["ec"]        = condutividade;
  s["ph"]        = phSolo;
  s["n"]         = nitrogenio;
  s["p"]         = fosforo;
  s["k"]         = potassio;
  s["temp_ar"]   = temperaturaAr;
  s["umid_ar"]   = umidadeAr;
  String jsonStr;
  serializeJson(doc, jsonStr);

  HTTPClient http;
  http.begin(GOOGLE_SCRIPT_URL);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonStr);
  Serial.printf("  [Sheets] HTTP %d\n", code);
  http.end();
  return (code == 200 || code == 302);
}

bool postParaSupabase(int indice) {
  StaticJsonDocument<512> doc;
  doc["dispositivo_id"]      = DISP_ID;
  doc["versao_fw"]           = VERSAO_FW;
  doc["ciclo"]               = ciclo;
  doc["pacote"]              = indice + 1;
  doc["sensor_pos"]          = 1;
  doc["umid_solo"]           = umidadeSolo;
  doc["temp_solo"]           = tempSolo;
  doc["ec"]                  = condutividade;
  doc["ph"]                  = phSolo;
  doc["n_mg_kg"]             = nitrogenio;
  doc["p_mg_kg"]             = fosforo;
  doc["k_mg_kg"]             = potassio;
  doc["temp_ar"]             = temperaturaAr;
  doc["umid_ar"]             = umidadeAr;
  doc["pluviometro_pulsos"]  = pluviometroLido;
  doc["voltagem_bateria"]    = voltagemBateria;
  doc["sinal_wifi_pct"]      = rssiParaPct(WiFi.RSSI());
  String jsonStr;
  serializeJson(doc, jsonStr);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, SUPABASE_URL);
  http.setTimeout(10000);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  int code = http.POST(jsonStr);
  Serial.printf("  [Supabase] HTTP %d\n", code);
  if (code != 201 && code > 0) {
    String r = http.getString();
    if (r.length() < 300) Serial.println(r);
  }
  http.end();
  return (code == 201);
}

void dormir() {
  Serial.printf("Dormindo %llus...\n", SLEEP_SEGUNDOS);
  digitalWrite(RELE_PIN, HIGH);
  digitalWrite(VEXT_PIN, HIGH);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUVIOMETRO_PIN, 0);
  esp_sleep_enable_timer_wakeup(SLEEP_SEGUNDOS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // desabilita brownout detector
  Serial.begin(115200);
  delay(500);

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW); delay(500);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, LOW);
  pinMode(RS485_DE_RE, OUTPUT); postTransmission();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.begin(1, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  dht.begin();
  pinMode(PLUVIOMETRO_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PLUVIOMETRO_PIN), pluviometroISR, FALLING);

  esp_sleep_wakeup_cause_t motivo = esp_sleep_get_wakeup_cause();
  if (motivo == ESP_SLEEP_WAKEUP_UNDEFINED) pluviometroPulsos = 0;  // primeiro boot

  // Acordou por pulso de pluviometro: so conta e volta a dormir.
  // TESTE: re-arma o timer de 1h. Em producao trocar por tracking de tempo
  // acumulado pra nao resetar o ciclo a cada virada.
  if (motivo == ESP_SLEEP_WAKEUP_EXT0) {
    pluviometroPulsos++;
    Serial.printf("Wake por pluviometro. Pulsos acumulados: %u. Voltando a dormir.\n",
                  pluviometroPulsos);
    dormir();
  }

  // Wake por timer ou primeiro boot: ciclo completo
  ciclo++;
  Serial.printf("\n=========================================\n");
  Serial.printf("%s | FW v%s | Ciclo %u\n", DEVICE_CODIGO, VERSAO_FW, ciclo);
  Serial.printf("=========================================\n");

  Serial.println("Aguardando sensores estabilizarem (3s)...");
  delay(3000);

  // Leitura de aquecimento (descartada)
  Serial.println("--- Leitura de aquecimento (descartada) ---");
  lerSensores();
  delay(2000);
  Serial.println("--- Sensores aquecidos, comecando ciclo real ---");

  bool algumEnvioOk = false;
  for (int i = 0; i < NUM_ENVIOS; i++) {
    Serial.printf("\n--- Envio %d/%d ---\n", i + 1, NUM_ENVIOS);
    lerSensores();
    if (conectarWiFi()) {
      bool okSheets = postParaSheets(i);
      bool okSupa   = postParaSupabase(i);
      if (okSheets || okSupa) algumEnvioOk = true;
      // reseta contador de pluviometro apos primeiro envio aceito
      if ((okSheets || okSupa) && i == 0) {
        noInterrupts(); pluviometroPulsos = 0; interrupts();
        Serial.println("  (contador de pluviometro zerado)");
      }
    }
    if (i < NUM_ENVIOS - 1) {
      Serial.printf("Aguardando %dms ate proximo envio...\n", MS_ENTRE_ENVIOS);
      delay(MS_ENTRE_ENVIOS);
    }
  }

  // OTA no final (depois de ler+enviar)
  Serial.println("\n--- Verificando OTA ---");
  verificarOTA();

  Serial.printf("\nCiclo %u completo.\n", ciclo);
  dormir();
}

void loop() {}
