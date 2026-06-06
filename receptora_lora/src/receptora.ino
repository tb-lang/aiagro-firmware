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
// v11 (jun/2026): SEM light sleep (a v10 com light sleep + DIO0 wake travou
// em campo — receptora dormiu e nao acordou). Modelo: ESP32 sempre acordado,
// WiFi OFF entre pacotes. Quando chega pacote LoRa, liga WiFi (timeout 5s,
// nao 20s), posta no Supabase, desliga WiFi. Mais robusto que light sleep.
// Consumo medio: ~30mA (LoRa RX + ESP32 active + WiFi off).
// Mantem TESTE_FAZENDA_EST2 + leitura bateria propria + voltagem_receptora.
#define VERSAO_FW "12"

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

// ====== Multi-estacao: mapa origem -> UUID Supabase ======
// Receptora pode atender VARIAS estacoes no ar. Cada pacote LoRa traz "origem"
// (string), e procuramos aqui o dispositivo_id certo pra subir pro Supabase.
// Se MULTI_ESTACOES estiver definido, usa esse mapa; senao usa ESTACAO_ESPERADA
// + DISP_ID_ESTACAO (modo single-estacao, retro-compativel).
struct EstacaoMap {
  const char* origem;
  const char* uuid;
};
static const EstacaoMap ESTACOES_ATENDIDAS[] = {
#ifdef MULTI_ESTACOES
  { "UNIUBE_LORA_CAFE",  "92c33471-df32-487e-a895-f044fa280def" },
  { "UNIUBE_PIVOT_EST1", "19ac767a-1c2a-4257-a7fa-0afdea4b9b27" },
#else
  { ESTACAO_ESPERADA, DISP_ID_ESTACAO },
  #ifdef TESTE_FAZENDA_EST2
  // Estacao secundaria pra TESTE DE ALCANCE LoRa.
  // Usa o MESMO UUID da estacao principal — distinguir pelo campo versao_fw
  // (a estacao de teste deve compilar com VERSAO_FW="TESTE").
  { "FAZENDA_EST2", DISP_ID_ESTACAO },
  #endif
#endif
};
static const int NUM_ESTACOES = sizeof(ESTACOES_ATENDIDAS)/sizeof(ESTACOES_ATENDIDAS[0]);

const char* getDispIdFor(const char* origem) {
  for (int i = 0; i < NUM_ESTACOES; i++) {
    if (strcmp(origem, ESTACOES_ATENDIDAS[i].origem) == 0) return ESTACOES_ATENDIDAS[i].uuid;
  }
  return NULL;
}

// ====== Pinos ======
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  27
#define LED_PIN     2
#define VOLT_PIN   34   // ADC1_CH6 — divisor da bateria 12V da receptora

// Le ADC raw da bateria (media de 4 amostras)
uint16_t lerBateriaRaw() {
  analogSetAttenuation(ADC_11db);
  uint32_t soma = 0;
  for (int i = 0; i < 4; i++) { soma += analogRead(VOLT_PIN); delay(5); }
  return soma / 4;
}

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

// ====== Heartbeat de bateria da receptora (1x/dia) ======
// Independe de pacote de estacao: mesmo que NENHUMA estacao mande nada, a
// receptora se reporta 1x/dia com a propria bateria. Assim da pra ver no
// Supabase que ela esta viva e com carga.
const unsigned long INTERVALO_BATERIA_MS = 24UL * 3600UL * 1000UL; // 24h
unsigned long ultimaBateria = 0;

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
  // Timeout 8s (era 20s) — desistir cedo pra nao bloquear LoRa
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200); Serial.print(".");
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
  // cache-buster: ?cb=<random> evita o cache distribuido do CDN do GitHub
  // (sem isso, maquinas diferentes do POP servem versoes diferentes por ~5 min)
  String urlVersion = String(OTA_URL_VERSION) + "?cb=" + String(esp_random());
  if (!http.begin(client, urlVersion)) {
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
  String urlBin = OTA_URL_BINARIO + "?cb=" + String(esp_random());
  Serial.printf("OTA: baixando %s\n", urlBin.c_str());
  WiFiClientSecure clientUpdate;
  clientUpdate.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(clientUpdate, urlBin);
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

// ====== Heartbeat: posta SO a bateria da propria receptora ======
// Liga WiFi, le a bateria da receptora e posta em `leituras` com
// dispositivo_id = a propria receptora (sensores zerados). Desliga WiFi no fim.
// Chamado no boot e a cada 24h — NAO depende de pacote de estacao.
void enviarBateriaReceptora() {
  Serial.println("\n[BATERIA] Heartbeat da receptora (independe de estacao)...");
  if (!conectarWiFi()) {
    Serial.println("[BATERIA] sem WiFi, pulando (tenta de novo em 24h)");
    return;
  }
  uint16_t batRaw = lerBateriaRaw();
  int sinalWifi = (WiFi.status() == WL_CONNECTED)
                  ? rssiParaPct(WiFi.RSSI(), -100, -30) : 0;

  StaticJsonDocument<384> doc;
  doc["dispositivo_id"]     = DISP_ID_RECEPTORA;   // a PROPRIA receptora
  doc["receptora_id"]       = DISP_ID_RECEPTORA;
  doc["versao_fw"]          = VERSAO_FW;
  doc["ciclo"]              = 0;
  doc["pacote"]             = 0;
  doc["sensor_pos"]         = 0;
  doc["voltagem_bateria"]   = batRaw;   // bateria da receptora (mesmo valor nos 2 campos)
  doc["voltagem_receptora"] = batRaw;
  doc["sinal_lora_pct"]     = 0;
  doc["sinal_wifi_pct"]     = sinalWifi;
  String json;
  serializeJson(doc, json);
  Serial.printf("[BATERIA] raw=%u wifi=%d%% -> postando...\n", batRaw, sinalWifi);
  if (postParaSupabase(json)) Serial.println("[BATERIA] OK");
  else                        Serial.println("[BATERIA] FALHOU");

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(50);
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

  // FILTRO: so processa pacotes de origens mapeadas
  String origem = docIn["origem"] | "";
  const char* dispIdEstacao = getDispIdFor(origem.c_str());
  if (dispIdEstacao == NULL) {
    Serial.printf("IGNORADO: origem=%s nao esta mapeada\n", origem.c_str());
    totalIgnorados++;
    return;
  }
  Serial.printf("OK: origem=%s -> disp_id=%s\n", origem.c_str(), dispIdEstacao);

  // Liga WiFi agora pra postar (light sleep deixa WiFi off entre pacotes)
  conectarWiFi();

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
  docSupa["dispositivo_id"]  = dispIdEstacao;   // resolvido pelo mapa
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
  docSupa["temp_ar"]            = docIn["temp_ar"] | 0.0;
  docSupa["umid_ar"]            = docIn["umid_ar"] | 0.0;
  docSupa["pluviometro_pulsos"] = docIn["pluv"]    | 0;     // repassa do pacote LoRa
  docSupa["voltagem_bateria"]   = docIn["bat"]     | 0;     // repassa do pacote LoRa (ADC raw)
  docSupa["voltagem_receptora"] = lerBateriaRaw();          // bateria da PROPRIA receptora
  docSupa["sinal_lora_pct"]     = sinalLora;
  docSupa["sinal_wifi_pct"]     = sinalWifi;
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

  // Desliga WiFi pra dormir economizando bateria ate proximo pacote
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(50);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=========================================");
  Serial.printf("RECEPTORA %s | FW v%s (wifi off entre pacotes)\n", DEVICE_CODIGO, VERSAO_FW);
  Serial.printf("Atende %d estacao(oes):\n", NUM_ESTACOES);
  for (int i = 0; i < NUM_ESTACOES; i++) {
    Serial.printf("  - %s -> %s\n", ESTACOES_ATENDIDAS[i].origem, ESTACOES_ATENDIDAS[i].uuid);
  }
  Serial.println("=========================================");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  iniciarLoRa();
  conectarWiFi();

  // OTA check no boot
  verificarOTA();
  ultimaCheckOTA = millis();

  // Heartbeat inicial: ja reporta a bateria da receptora no boot
  enviarBateriaReceptora();
  ultimaBateria = millis();

  // Desliga WiFi - so liga quando chegar pacote LoRa.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  Serial.println("Aguardando pacotes (LoRa RX continuo, WiFi off)...");
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
    // processarEEnviar liga WiFi, posta, e DESLIGA WiFi no fim
    processarEEnviar(payload, rssi, snr);
    LoRa.receive();
  }

  // OTA check periodico (a cada 6h)
  if (millis() - ultimaCheckOTA > INTERVALO_OTA_MS) {
    Serial.println("\n[OTA] Check periodico de 6h...");
    conectarWiFi();
    verificarOTA();
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    ultimaCheckOTA = millis();
    LoRa.receive();
  }

  // Heartbeat de bateria da receptora (a cada 24h, independente de estacao)
  if (millis() - ultimaBateria > INTERVALO_BATERIA_MS) {
    enviarBateriaReceptora();
    ultimaBateria = millis();
    LoRa.receive();
  }

  delay(20);
}
