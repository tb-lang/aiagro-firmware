/**
 * UNIUBE LARANJA WiFi - projeto independente (separado do laranja_v44L em 06/jun)
 * Base: V44 da Bela Vista (lógica de leitura testada e validada)
 * + OTA via GitHub
 * + POST direto pro Supabase (sem Sheets, sem LoRa, sem receptora)
 * + Pluviometro Hall A3144 com EXT0 wakeup (conta viradas durante deep sleep)
 *
 * Pinagem AI Agro custom:
 *   RS485 TX 17 | RX 16 | DE/RE 32 | DHT22 4 | RELE 26 | VEXT 0
 *   PLUV 25 | VOLT 34 | OLED SDA 21 SCL 22
 *
 * Sensor 7x1 LOTE VELHO (Bela/Laranja/Cafe):
 *   0x0012 umid (/10)  0x0014 temp (/10)  0x0015 EC  0x0007 pH (formula custom)
 *   0x001E NPK
 *
 * Ciclo: acorda no horario agendado (07:30 BR) ou por virada do pluviometro.
 *  - Wake timer/normal: 3 envios espacados 60s + dorme ate proximo dia
 *  - Wake EXT0 (virada): so incrementa contador no RTC e volta a dormir ATE O
 *    HORARIO AGENDADO (sem WiFi, sem rele). l4 porta o b5 da Bela: no l3 o
 *    timer de 60s depois da virada caia em ehHoraEnvio (qualquer hora depois
 *    das 07:30) e virava um ciclo completo de envio — foi isso que esgotou a
 *    bateria da Bela na chuva de 30/set–04/out.
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
#include "time.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ====== Versao do firmware (sincronizar com arquivo VERSION do repo) ======
// ATENCAO: se subir o arquivo VERSION sem subir esta constante, a placa entra
// em LOOP INFINITO de OTA. Os dois TEM que andar juntos.
#define VERSAO_FW "l4"

// ====== Config por dispositivo (defaults; sobrescritos por build_flags) ======
#ifndef DEVICE_CODIGO
  #define DEVICE_CODIGO "UNIUBE_WIFI_LARANJA"
#endif
#ifndef DISP_ID
  #define DISP_ID "efea0039-0864-4fce-b256-56f54b5c9b1c"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID "AP 101"
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS "Barbosan"
#endif
// Rede reserva (l3): se a principal falhar, tenta esta. Vazio = sem fallback.
// Usada na virada WIFI-UNIUBE -> AiAgro pra estacao nunca ficar sem OTA.
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
  "https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/laranja_wifi/VERSION";
const String OTA_URL_BINARIO =
  String("https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/laranja_wifi/builds/")
  + DEVICE_CODIGO + ".bin";

// ====== Horario agendado de envio (Brasilia UTC-3) ======
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = -3 * 3600;
const int   daylightOffset_sec = 0;
#define NUMERO_DE_ENVIOS       10   // MODO TESTE (l2): 10 leituras seguidas p/ ver se batem
#define HORA_ENVIO_AGENDADO    7
#define MINUTO_ENVIO_AGENDADO  30
// Sem hora certa (nunca pegou NTP desde que ligou), tenta de novo em 3h.
// No l3 era 1h, e cada tentativa e um ciclo completo (rele + WiFi).
#define SONO_SEM_HORA_S        (3 * 3600L)

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
RTC_DATA_ATTR uint32_t pluviometroPulsos = 0;
RTC_DATA_ATTR uint32_t ciclo = 0;
// Agenda (l4, igual b5 da Bela). O relogio do sistema continua contando no
// deep sleep, entao proximoEnvio (no mesmo relogio) vale mesmo sem WiFi.
// timerEhRearme marca que o proximo wake por timer e so pra rearmar o
// pluviometro, NAO pra enviar.
RTC_DATA_ATTR time_t  proximoEnvio    = 0;
RTC_DATA_ATTR bool    timerEhRearme   = false;
bool wifiDesistiu = false;   // no ciclo atual o WiFi ja falhou -> nao insiste
volatile unsigned long ultimoPulsoMs = 0;
uint32_t pluviometroPulsosLidos = 0;
float temperaturaAr=0, umidadeAr=0, umidadeSolo=0, tempSolo=0, phSolo=0, condutividade=0;
int nitrogenio=0, fosforo=0, potassio=0;
float voltagemBateria = 0;

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

bool conectarWiFiReal(bool forcar);
bool conectarWiFi(bool forcar = false) {
  // l4: se a rede ja falhou neste ciclo, nao tenta de novo nos envios
  // seguintes nem no OTA — cada tentativa e radio ligado gastando bateria.
  if (wifiDesistiu) return false;
  bool ok = conectarWiFiReal(forcar);
  if (!ok) wifiDesistiu = true;
  return ok;
}

bool conectarWiFiReal(bool forcar) {
  // forcar=true: derruba a sessao atual e reconecta do ZERO. Usado a cada um
  // dos 3 envios — em sinal fraco (Bela/Olimpia) a sessao as vezes fica
  // "presa" em WL_CONNECTED mas nao transmite; reconexao limpa garante POST.
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

void lerSensores() {
  analogSetAttenuation(ADC_11db);
  voltagemBateria = analogRead(VOLTIMETRO_PIN);
  temperaturaAr   = dht.readTemperature();
  umidadeAr       = dht.readHumidity();
  if (isnan(temperaturaAr)) temperaturaAr = 0;
  if (isnan(umidadeAr))     umidadeAr     = 0;

  if (node.readHoldingRegisters(0x0012, 2) == node.ku8MBSuccess) {
    umidadeSolo = node.getResponseBuffer(0) / 10.0;
    tempSolo    = node.getResponseBuffer(1) / 10.0;
  }
  if (node.readHoldingRegisters(0x0015, 1) == node.ku8MBSuccess) condutividade = node.getResponseBuffer(0);
  if (node.readHoldingRegisters(0x0007, 1) == node.ku8MBSuccess) {
    // pH: formula calibrada do V44 (Bela Vista)
    int leituraPH = node.getResponseBuffer(0);
    phSolo = 5.5 + ((leituraPH - 2432.0) * 3.0) / (2666.0 - 2432.0);
    phSolo = constrain(phSolo, 3.0, 10.0);
  }
  if (node.readHoldingRegisters(0x001E, 3) == node.ku8MBSuccess) {
    nitrogenio = node.getResponseBuffer(0);
    fosforo    = node.getResponseBuffer(1);
    potassio   = node.getResponseBuffer(2);
  }
  noInterrupts(); pluviometroPulsosLidos = pluviometroPulsos; interrupts();

  Serial.printf("[LIDO] ar=%.1fC/%.1f%%  solo=%.1f%%/%.1fC EC=%.0f pH=%.2f NPK=%d/%d/%d bat=%.0f pluv=%u\n",
    temperaturaAr, umidadeAr, umidadeSolo, tempSolo, condutividade, phSolo,
    nitrogenio, fosforo, potassio, voltagemBateria, pluviometroPulsosLidos);
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
  doc["ec"]                  = (int)condutividade;
  doc["ph"]                  = phSolo;
  doc["n_mg_kg"]             = nitrogenio;
  doc["p_mg_kg"]             = fosforo;
  doc["k_mg_kg"]             = potassio;
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
  Serial.printf("[Supabase] HTTP %d\n", code);
  if (code != 201 && code > 0) {
    String resp = http.getString();
    if (resp.length() < 300) Serial.println(resp);
  }
  http.end();
  return (code == 201);
}

// Dorme depois de uma virada, sem enviar. O EXT0 e por NIVEL: se a bascula
// parou com o ima em cima do sensor (pino em LOW), religar o EXT0 acordaria na
// hora e contaria de novo. Entao espera o pino soltar; se nao soltar, dorme so
// com timer e acorda pra rearmar (timerEhRearme) — esse wake nao envia nada.
void dormirSoContando(long falta) {
  unsigned long t0 = millis();
  while (digitalRead(PLUVIOMETRO_PIN) == LOW && millis() - t0 < 1500) delay(10);
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  display.ssd1306_command(SSD1306_DISPLAYOFF); digitalWrite(VEXT_PIN, HIGH);
  if (digitalRead(PLUVIOMETRO_PIN) == LOW) {
    long s = falta < 60 ? falta : 60;
    timerEhRearme = true;
    esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
  } else {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUVIOMETRO_PIN, 0);
    esp_sleep_enable_timer_wakeup((uint64_t)falta * 1000000ULL);
  }
  esp_deep_sleep_start();
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
  node.begin(1, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  pinMode(PLUVIOMETRO_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PLUVIOMETRO_PIN), pluviometroISR, FALLING);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUVIOMETRO_PIN, 0);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) pluviometroPulsos++;
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {   // ligou do zero
    pluviometroPulsos = 0; proximoEnvio = 0; timerEhRearme = false;
  }

  ciclo++;
  Serial.printf("\n=== %s V%s | Ciclo %u | wakeup=%d ===\n",
    DEVICE_CODIGO, VERSAO_FW, ciclo, (int)wakeup_reason);

  struct tm timeinfo; bool horaOk = false;
  bool wakeTimer = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER);
  bool wakeChuva = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  // Timer que so servia pra rearmar o pluviometro NAO e hora de enviar.
  bool soRearme = wakeTimer && timerEhRearme;
  timerEhRearme = false;

  // Virada (ou rearme): conta e volta a dormir ate o horario agendado, sem
  // ligar WiFi nem rele. So segue pro envio se a agenda ja venceu.
  // (l3 ligava o WiFi em TODA virada so pra pegar a hora no NTP.)
  if (wakeChuva || soRearme) {
    long falta = (proximoEnvio > 0) ? (long)(proximoEnvio - time(nullptr)) : 0;
    if (proximoEnvio > 0 && falta > 30) {
      Serial.printf("[PLUV] virada %u contada, faltam %lds pro envio\n", pluviometroPulsos, falta);
      dormirSoContando(falta);   // nao volta
    }
    Serial.println("[PLUV] agenda vencida, segue pro envio");
  }

  // Daqui pra baixo e sempre envio: power-on, timer agendado ou virada que
  // caiu na hora do envio. O sono ja foi calculado pra acordar no horario —
  // nao depende de NTP (no l3, sem hora o ehHoraEnvio barrava o envio).
  mostrarStatus("CONECTANDO...");
  digitalWrite(RELE_PIN, LOW);   // liga sensor 7x1
  delay(2500);
  for (int i = 0; i < NUMERO_DE_ENVIOS; i++) {
    if (conectarWiFi(true)) {   // reconexao limpa a cada leitura
      mostrarStatus("ENVIANDO " + String(i+1) + "/" + String(NUMERO_DE_ENVIOS));
      lerSensores();
      postParaSupabase(i);
    } else {
      Serial.println("Sem WiFi neste ciclo, encerrando envios");
      break;   // l4: sem rede nao le nem envia; nao fica com rele ligado a toa
    }
    if (i < NUMERO_DE_ENVIOS - 1) delay(15000);   // MODO TESTE (l2): 15s entre leituras
  }
  digitalWrite(RELE_PIN, HIGH);   // desliga sensor
  mostrarStatus("CHECANDO OTA");
  verificarOTA();

  // Hora pro calculo do sono. Com WiFi, acerta pelo NTP. Sem WiFi, o relogio
  // do ESP32 continua certo se ja pegou NTP antes (sobrevive ao deep sleep;
  // so zera se a placa desligar) — no l3 isso era ignorado e caia em 1h.
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  if (getLocalTime(&timeinfo, WiFi.status() == WL_CONNECTED ? 5000 : 100)) horaOk = true;

  long tempo_sono = SONO_SEM_HORA_S;
  if (horaOk) {
    long seg_hoje = (timeinfo.tm_hour * 3600L) + (timeinfo.tm_min * 60L) + timeinfo.tm_sec;
    long seg_obj  = (HORA_ENVIO_AGENDADO * 3600L) + (MINUTO_ENVIO_AGENDADO * 60L);
    tempo_sono = (seg_hoje < seg_obj) ? (seg_obj - seg_hoje) : (86400L - seg_hoje + seg_obj);
  }
  proximoEnvio = time(nullptr) + tempo_sono;
  Serial.printf("Dormindo %lds ate proximo envio (%s)...\n", tempo_sono, horaOk ? "agendado" : "sem hora, fallback");
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  display.ssd1306_command(SSD1306_DISPLAYOFF); digitalWrite(VEXT_PIN, HIGH);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUVIOMETRO_PIN, 0);
  esp_sleep_enable_timer_wakeup((uint64_t)tempo_sono * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
