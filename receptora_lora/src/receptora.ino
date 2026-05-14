/**
 * RECEPTORA LORA - AI AGRO ESP32 + RA-02 (GENERICA + OTA)
 *
 * 1 source serve TODAS as receptoras. O que muda por dispositivo vem via
 * build_flags no platformio.ini:
 *   -DDEVICE_CODIGO         ex: "UNIUBE_PIVOT_RECEPTORA"
 *   -DESTACAO_ESPERADA      ex: "UNIUBE_PIVOT_EST1" (filtro de origem LoRa)
 *   -DDISP_ID_ESTACAO       UUID do dispositivo estacao no Supabase
 *   -DDISP_ID_RECEPTORA     UUID do dispositivo receptora no Supabase
 *   -DWIFI_SSID / -DWIFI_PASS
 *
 * OTA: checa GitHub no boot e a cada 6h. Se VERSION remoto != VERSAO_FW local,
 * baixa builds/<DEVICE_CODIGO>.bin e auto-flasha.
 *
 * Faz dual write: Google Sheets (legado) + Supabase (novo).
 *
 * PINOS (Placa AI Agro): LoRa SCK 18 | MISO 19 | MOSI 23 | SS 5 | RST 14 | DIO0 27 | LED 2
 */
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ====== Versao do firmware (sincronizar com arquivo VERSION do repo) ======
#define VERSAO_FW "2"

// ====== Config por dispositivo (defaults; sobrescritos por build_flags) ======
#ifndef DEVICE_CODIGO
  #define DEVICE_CODIGO "UNIUBE_PIVOT_RECEPTORA"
#endif
#ifndef ESTACAO_ESPERADA
  #define ESTACAO_ESPERADA "UNIUBE_PIVOT_EST1"
#endif
#ifndef DISP_ID_ESTACAO
  #define DISP_ID_ESTACAO "19ac767a-1c2a-4257-a7fa-0afdea4b9b27"
#endif
#ifndef DISP_ID_RECEPTORA
  #define DISP_ID_RECEPTORA "c84191fb-21cb-45b0-985e-69e137c2e8ad"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID "Ap6401-2G"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS "21012003"
#endif

// ====== Pinos ======
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  27
#define LED_PIN     2

// ====== Endpoints fixos ======
const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/macros/s/AKfycby9tF9b7n79DXCQ8qT47U_0Ao-9eiUhsNe6iDo8579LLQOscyLphv2C4UxR7Y6Orn3P/exec";
const char* SUPABASE_URL = "https://bwtotmprzmldczafjhrg.supabase.co/rest/v1/leituras";
const char* SUPABASE_ANON_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJ3dG90bXByem1sZGN6YWZqaHJnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzgwNzI0MjAsImV4cCI6MjA5MzY0ODQyMH0.-ZiY9JSdsUoCC2dSsesYemH-vN61Gl5odX9XrRxc-jo";

// ====== URLs OTA (derivadas do DEVICE_CODIGO) ======
const char* OTA_URL_VERSION =
  "https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/receptora_lora/VERSION";
const String OTA_URL_BINARIO =
  String("https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/receptora_lora/builds/")
  + DEVICE_CODIGO + ".bin";

// ====== Controle OTA ======
const unsigned long INTERVALO_OTA_MS = 6UL * 3600UL * 1000UL; // 6h
unsigned long ultimaCheckOTA = 0;

// ====== Estatistica ======
uint32_t totalRecebidos = 0;
uint32_t totalIgnorados = 0;
uint32_t totalSheetsOK = 0, totalSheetsFalha = 0;
uint32_t totalSupabaseOK = 0, totalSupabaseFalha = 0;

void piscarLED(int vezes, int ms) {
  for (int i = 0; i < vezes; i++) {
    digitalWrite(LED_PIN, HIGH); delay(ms);
    digitalWrite(LED_PIN, LOW); delay(ms);
  }
}

int rssiParaPct(int rssi, int rssiMin, int rssiMax) {
  if (rssi < rssiMin) rssi = rssiMin;
  if (rssi > rssiMax) rssi = rssiMax;
  return map(rssi, rssiMin, rssiMax, 0, 100);
}

void iniciarLoRa() {
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW);  delay(200);
  digitalWrite(LORA_RST, HIGH); delay(500);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  for (int i = 1; i <= 5; i++) {
    if (LoRa.begin(915E6)) {
      LoRa.setSpreadingFactor(7);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setCodingRate4(5);
      Serial.println("LoRa OK (915MHz / SF7 / BW125 / CR4/5)");
      piscarLED(3, 100);
      return;
    }
    Serial.printf("LoRa tentativa %d falhou\n", i);
    delay(500);
  }
  Serial.println("ERRO FATAL: LoRa nao iniciou");
  while (true) { piscarLED(1, 50); delay(200); }
}

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
    piscarLED(2, 200);
    return true;
  }
  Serial.println("WiFi FAIL");
  return false;
}

// ====== OTA ======
void verificarOTA() {
  if (!conectarWiFi()) {
    Serial.println("OTA: sem WiFi, pulando check");
    return;
  }
  Serial.printf("OTA: checando versao (atual: %s)...\n", VERSAO_FW);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, OTA_URL_VERSION)) {
    Serial.println("OTA: falha ao abrir URL de versao");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("OTA: HTTP %d ao ler VERSION, pulando\n", code);
    http.end();
    return;
  }
  String novaVersao = http.getString();
  novaVersao.trim();
  http.end();

  if (novaVersao == VERSAO_FW) {
    Serial.printf("OTA: ja na versao mais recente (%s)\n", VERSAO_FW);
    return;
  }

  Serial.printf("OTA: nova versao disponivel: %s (atual %s)\n", novaVersao.c_str(), VERSAO_FW);
  Serial.printf("OTA: baixando %s\n", OTA_URL_BINARIO.c_str());
  WiFiClientSecure clientUpdate;
  clientUpdate.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(clientUpdate, OTA_URL_BINARIO);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA: FALHOU (%d): %s\n",
        httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: sem update (inesperado)");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA: OK (vai reiniciar)");
      break;
  }
}

// ====== POSTs ======
bool postParaSheets(const String& jsonStr) {
  if (WiFi.status() != WL_CONNECTED) { if (!conectarWiFi()) return false; }
  HTTPClient http;
  http.begin(GOOGLE_SCRIPT_URL);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(jsonStr);
  Serial.printf("  [Sheets] HTTP %d\n", httpCode);
  http.end();
  return (httpCode == 200 || httpCode == 302);
}

bool postParaSupabase(const String& jsonStr) {
  if (WiFi.status() != WL_CONNECTED) { if (!conectarWiFi()) return false; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, SUPABASE_URL);
  http.setTimeout(10000);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  int httpCode = http.POST(jsonStr);
  Serial.printf("  [Supabase] HTTP %d\n", httpCode);
  if (httpCode != 201 && httpCode > 0) {
    String resp = http.getString();
    if (resp.length() < 300) Serial.println(resp);
  }
  http.end();
  return (httpCode == 201);
}

void processarEEnviar(const String& payload, int rssiLora, float snrLora) {
  Serial.println("\n=========================================");
  Serial.printf("RX | RSSI %d dBm | SNR %.1f | %d bytes\n",
                rssiLora, snrLora, payload.length());
  Serial.println(payload);

  StaticJsonDocument<384> docIn;
  DeserializationError err = deserializeJson(docIn, payload);
  if (err) {
    Serial.print("JSON invalido: "); Serial.println(err.c_str());
    return;
  }

  // FILTRO: so processa pacotes da estacao esperada
  String origem = docIn["origem"] | "";
  if (origem != ESTACAO_ESPERADA) {
    Serial.printf("IGNORADO: origem=%s, esperado=%s\n", origem.c_str(), ESTACAO_ESPERADA);
    totalIgnorados++;
    return;
  }

  int sinalLora = rssiParaPct(rssiLora, -120, -30);
  int sinalWifi = (WiFi.status() == WL_CONNECTED)
                  ? rssiParaPct(WiFi.RSSI(), -100, -30) : 0;

  // --- JSON Google Sheets (schema B) ---
  StaticJsonDocument<512> docSheets;
  docSheets["origem"]     = origem;
  docSheets["versao"]     = docIn["versao"] | "";
  docSheets["ciclo"]      = docIn["ciclo"]  | 0;
  docSheets["pacote"]     = docIn["pacote"] | 0;
  docSheets["sinal_lora"] = sinalLora;
  docSheets["sinal_wifi"] = sinalWifi;
  JsonArray arr = docSheets.createNestedArray("sensores");
  JsonObject s = arr.createNestedObject();
  s["id"]        = docIn["sensor"]    | 0;
  s["umid_solo"] = docIn["umid_solo"] | 0.0;
  s["temp_solo"] = docIn["temp_solo"] | 0.0;
  s["ec"]        = docIn["ec"]        | 0;
  s["ph"]        = docIn["ph"]        | 0.0;
  s["n"]         = docIn["n"]         | 0;
  s["p"]         = docIn["p"]         | 0;
  s["k"]         = docIn["k"]         | 0;
  s["temp_ar"]   = docIn["temp_ar"]   | 0.0;
  s["umid_ar"]   = docIn["umid_ar"]   | 0.0;
  String jsonSheets;
  serializeJson(docSheets, jsonSheets);

  // --- JSON Supabase ---
  StaticJsonDocument<512> docSupa;
  docSupa["dispositivo_id"]  = DISP_ID_ESTACAO;
  docSupa["receptora_id"]    = DISP_ID_RECEPTORA;
  docSupa["versao_fw"]       = docIn["versao"] | "";
  docSupa["ciclo"]           = docIn["ciclo"]  | 0;
  docSupa["pacote"]          = docIn["pacote"] | 0;
  docSupa["sensor_pos"]      = docIn["sensor"]    | 0;
  docSupa["umid_solo"]       = docIn["umid_solo"] | 0.0;
  docSupa["temp_solo"]       = docIn["temp_solo"] | 0.0;
  docSupa["ec"]              = docIn["ec"]        | 0;
  docSupa["ph"]              = docIn["ph"]        | 0.0;
  docSupa["n_mg_kg"]         = docIn["n"]         | 0;
  docSupa["p_mg_kg"]         = docIn["p"]         | 0;
  docSupa["k_mg_kg"]         = docIn["k"]         | 0;
  docSupa["temp_ar"]         = docIn["temp_ar"]   | 0.0;
  docSupa["umid_ar"]         = docIn["umid_ar"]   | 0.0;
  docSupa["sinal_lora_pct"]  = sinalLora;
  docSupa["sinal_wifi_pct"]  = sinalWifi;
  String jsonSupa;
  serializeJson(docSupa, jsonSupa);

  Serial.println("Mandando p/ Sheets...");
  if (postParaSheets(jsonSheets)) totalSheetsOK++; else totalSheetsFalha++;

  Serial.println("Mandando p/ Supabase...");
  if (postParaSupabase(jsonSupa)) totalSupabaseOK++; else totalSupabaseFalha++;

  Serial.printf("Stats: rx=%u ign=%u | Sheets ok=%u falha=%u | Supa ok=%u falha=%u\n",
                totalRecebidos, totalIgnorados,
                totalSheetsOK, totalSheetsFalha,
                totalSupabaseOK, totalSupabaseFalha);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=========================================");
  Serial.printf("RECEPTORA %s | FW v%s\n", DEVICE_CODIGO, VERSAO_FW);
  Serial.printf("Filtro: aceita so pacotes de %s\n", ESTACAO_ESPERADA);
  Serial.println("=========================================");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  iniciarLoRa();
  conectarWiFi();

  // OTA check no boot (ANTES de entrar no loop de receber dados)
  verificarOTA();
  ultimaCheckOTA = millis();

  Serial.println("Aguardando pacotes...");
  LoRa.receive();
}

void loop() {
  int tamanho = LoRa.parsePacket();
  if (tamanho > 0) {
    totalRecebidos++;
    piscarLED(2, 100);
    String payload = "";
    while (LoRa.available()) payload += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();
    processarEEnviar(payload, rssi, snr);
    LoRa.receive();
  }

  // OTA check periodico (a cada 6h, so quando nao tem pacote chegando)
  if (millis() - ultimaCheckOTA > INTERVALO_OTA_MS) {
    Serial.println("\n[OTA] Check periodico de 6h...");
    verificarOTA();
    ultimaCheckOTA = millis();
    LoRa.receive();
  }

  delay(20);
}
