/**
 * AIAGRO — ESTACAO WiFi DO CLIENTE (modelo velho, sensor 7x1 LOTE VELHO)
 * Base: laranja_wifi l3 (logica de leitura testada e validada em campo)
 *
 * O QUE MUDA EM RELACAO AS ESTACOES ATUAIS
 *   Nao existe mais SSID/senha compilados. O produtor configura a internet
 *   dele pelo celular, num portal igual ao de hotel (ver provisao.cpp).
 *   O mesmo binario serve pra qualquer cliente do lote.
 *
 * COMO O PRODUTOR ABRE O PORTAL (sem notebook, sem tecnico)
 *   a) primeira vez: automatico, nao ha rede salva;
 *   b) liga e desliga a estacao 3x seguidas (cada vez ~5s ligada);
 *   c) botao CONFIG opcional (GPIO33 -> GND) segurado 5s;
 *   d) automatico depois de 3 ciclos seguidos sem conseguir conectar
 *      (foi o caso da Laranja quando a WIFI-UNIUBE saiu do ar).
 *   Em qualquer um: sobe a rede "AiAgro-XXXX", senha aiagro123.
 *
 * Pinagem AI Agro custom (identica a das estacoes em campo):
 *   RS485 TX 17 | RX 16 | DE/RE 32 | DHT22 4 | RELE 26 | VEXT 0
 *   PLUV 25 | VOLT 34 | (opcional) BOTAO CONFIG 33 | (opcional) LED 21
 *
 * SEM OLED (e3). Nenhuma placa AiAgro leva display — ele come bateria numa
 * estacao que precisa durar meses. O que era tela virou log no serial e, se
 * alguem soldar um LED, sinal luminoso (-DLED_STATUS=21; 21 e 22 ficaram
 * livres com a saida do I2C). Sem LED a estacao pedindo WiFi so se anuncia
 * pela rede "AiAgro-XXXX" na lista do celular — por isso o nome dela vai
 * impresso na etiqueta.
 *
 * Sensor 7x1 — DOIS modelos, detectados sozinho no primeiro ciclo (e5):
 *   LOTE NOVO (o do lote de 20, medido no mbpoll em 02/set): 4800 baud, bloco
 *     0x0000: umid(/10) temp(/10) EC pH(/10) N P K
 *   LOTE VELHO (Bela/Laranja/Cafe): 9600 baud, 0x0012 umid+temp (/10),
 *     0x0015 EC, 0x0007 pH (formula V44), 0x001E NPK
 *   Existe ainda uma 3a variante (pivo Cristalina): 0x0000 com temp/umid
 *   TROCADOS e pH /100 — por isso nunca copiar registrador de outra nota sem
 *   conferir no mbpoll antes.
 *
 * Ciclo: acorda no horario agendado (07:30 BR) ou por virada do pluviometro.
 *  - Wake timer/normal: N envios espacados + dorme ate o proximo dia
 *  - Wake EXT0 (virada): so incrementa contador no RTC e dorme 60s
 */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <ModbusMaster.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "time.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "provisao.h"

// ====== Versao do firmware (sincronizar com o arquivo VERSION do repo) ======
#define VERSAO_FW "e5"

// ====== Config por dispositivo (defaults; sobrescritos por build_flags) ======
#ifndef DEVICE_CODIGO
  #define DEVICE_CODIGO "ESTACAO_CLIENTE"
#endif
// UUID de fabrica. Vale so como reserva: o valor bom e o gravado na NVS
// (comando de bancada "id=<uuid>", o mesmo do QR da etiqueta).
#ifndef DISP_ID
  #define DISP_ID ""
#endif

// ====== Endpoints fixos ======
const char* SUPABASE_URL = "https://bwtotmprzmldczafjhrg.supabase.co/rest/v1/leituras";
const char* SUPABASE_ANON_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJ3dG90bXByem1sZGN6YWZqaHJnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzgwNzI0MjAsImV4cCI6MjA5MzY0ODQyMH0.-ZiY9JSdsUoCC2dSsesYemH-vN61Gl5odX9XrRxc-jo";

// ====== URLs OTA ======
const char* OTA_URL_VERSION =
  "https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/estacao_cliente/VERSION";
const String OTA_URL_BINARIO =
  String("https://raw.githubusercontent.com/tb-lang/aiagro-firmware/main/estacao_cliente/builds/")
  + DEVICE_CODIGO + ".bin";

// ====== Horario agendado de envio (Brasilia UTC-3) ======
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = -3 * 3600;
const int   daylightOffset_sec = 0;
#define HORA_ENVIO_AGENDADO    7
#define MINUTO_ENVIO_AGENDADO  30

#ifdef MODO_TESTE
  #define NUMERO_DE_ENVIOS   10       // bancada: 10 leituras seguidas
  #define INTERVALO_ENVIO_MS 15000    // a cada 15s
  #define SONO_TESTE_SEG     600      // e volta em 10 min (nao espera 07:30)
#else
  #define NUMERO_DE_ENVIOS   3        // producao: 3 pacotes as 07:30
  #define INTERVALO_ENVIO_MS 60000    // 60s entre eles
#endif

// ====== Pinos ======
#define VEXT_PIN        0
#define DHTPIN          4
#define DHTTYPE         DHT22
#define RS485_TX        17
#define RS485_RX        16
#define RS485_DE_RE     32
#define RELE_PIN        26
#define VOLTIMETRO_PIN  34
#define PLUVIOMETRO_PIN 25
#define BOTAO_CONFIG    255   // 33 se soldar o botao; 255 = sem botao
#ifndef LED_STATUS
  #define LED_STATUS    255   // 21 se soldar um LED (com resistor); 255 = sem LED
#endif

// ====== Globais ======
DHT dht(DHTPIN, DHTTYPE);
ModbusMaster node;
Preferences nvs;
RTC_DATA_ATTR uint32_t pluviometroPulsos = 0;
RTC_DATA_ATTR uint32_t ciclo = 0;
RTC_DATA_ATTR char     otaAlvo[8] = {0};    // versao que o OTA esta perseguindo
RTC_DATA_ATTR uint8_t  otaTentativas = 0;
#define OTA_MAX_TENTATIVAS 3
volatile unsigned long ultimoPulsoMs = 0;
uint32_t pluviometroPulsosLidos = 0;
float temperaturaAr=0, umidadeAr=0, umidadeSolo=0, tempSolo=0, phSolo=0, condutividade=0;
int nitrogenio=0, fosforo=0, potassio=0;
float voltagemBateria = 0;
String meuId;

void IRAM_ATTR pluviometroISR() {
  unsigned long agora = millis();
  if (agora - ultimoPulsoMs > 250) { pluviometroPulsos++; ultimoPulsoMs = agora; }
}

void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW);  }

void led(bool on) {
  if (LED_STATUS != 255) digitalWrite(LED_STATUS, on ? HIGH : LOW);
}

// Era a tela do OLED. Sem display, o estado da estacao vive no serial —
// e o que o campo enxerga e o LED (se soldado) e a rede que ela sobe.
void mostrarStatus(String texto) {
  texto.replace("\n", " | ");
  Serial.printf("[estado] %s\n", texto.c_str());
}

// Portal no ar. Sem tela, quem avisa o produtor e a etiqueta (que traz o nome
// da rede e a senha) e, se existir, o LED aceso.
void mostrarPortal(const String& ap, const char* senha) {
  led(true);
  Serial.printf("[portal] no ar — rede \"%s\" senha \"%s\" (http://192.168.4.1)\n",
                ap.c_str(), senha);
}

// ====== Contador de falhas de conexao (NVS) ======
// Depois de 3 ciclos seguidos sem entrar em nenhuma rede salva, a estacao
// para de insistir calada e volta a chamar pelo portal. Foi exatamente o
// buraco em que a Laranja caiu quando a WIFI-UNIUBE foi desligada.
uint8_t incrementaFalhas() {
  nvs.begin("aiagro_est", false);
  uint8_t n = nvs.getUChar("falhas", 0) + 1;
  nvs.putUChar("falhas", n);
  nvs.end();
  return n;
}
void zeraFalhas() {
  nvs.begin("aiagro_est", false);
  if (nvs.getUChar("falhas", 0) != 0) nvs.putUChar("falhas", 0);
  nvs.end();
}

// ====== OTA: helpers de versao (portados do bela_wifi b3) ======
// Formato do projeto: prefixo de letra + numero ("e4", "b12") ou so numero.
// Prefixo separa o projeto; numero e a ordem.
static int versaoNumero(const String& v) {
  int i = 0;
  while (i < (int)v.length() && !isDigit(v[i])) i++;
  if (i >= (int)v.length()) return -1;
  for (int k = i; k < (int)v.length(); k++)
    if (!isDigit(v[k])) return -1;
  return v.substring(i).toInt();
}

static String versaoPrefixo(const String& v) {
  int i = 0;
  while (i < (int)v.length() && !isDigit(v[i])) i++;
  return v.substring(0, i);
}

// ====== OTA ======
void verificarOTA() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("OTA: sem WiFi, pulando"); return; }
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
    otaTentativas = 0; otaAlvo[0] = 0;   // chegou no alvo, limpa a trava
    return;
  }

  // So atualiza pra FRENTE. Com o teste "!=" que estava aqui, um VERSION
  // defasado no GitHub rebaixaria as 20 placas em campo sem ninguem perceber.
  String atual = String(VERSAO_FW);
  int nNova = versaoNumero(novaVersao), nAtual = versaoNumero(atual);
  if (nNova < 0 || nAtual < 0) {
    Serial.printf("OTA: versao ilegivel (remota '%s', atual '%s'), pulando\n",
                  novaVersao.c_str(), atual.c_str());
    return;
  }
  if (versaoPrefixo(novaVersao) != versaoPrefixo(atual)) {
    Serial.printf("OTA: prefixo diferente (remota '%s' x atual '%s') - projeto errado, pulando\n",
                  novaVersao.c_str(), atual.c_str());
    return;
  }
  if (nNova < nAtual) {
    Serial.printf("OTA: remota %s e MAIS VELHA que %s - downgrade bloqueado\n",
                  novaVersao.c_str(), atual.c_str());
    return;
  }

  // Trava anti-loop: se o bin publicado esta com VERSAO_FW errada, a placa
  // baixa, grava, continua anunciando a versao velha e baixa de novo, pra
  // sempre. Depois de OTA_MAX_TENTATIVAS no mesmo alvo, desiste.
  if (strncmp(otaAlvo, novaVersao.c_str(), sizeof(otaAlvo) - 1) == 0) {
    if (otaTentativas >= OTA_MAX_TENTATIVAS) {
      Serial.printf("OTA: %s ja tentada %d vezes sem efeito - desistindo "
                    "(bin publicado provavelmente com VERSAO_FW errada)\n",
                    novaVersao.c_str(), otaTentativas);
      return;
    }
    otaTentativas++;
  } else {
    strncpy(otaAlvo, novaVersao.c_str(), sizeof(otaAlvo) - 1);
    otaAlvo[sizeof(otaAlvo) - 1] = 0;
    otaTentativas = 1;
  }

  Serial.printf("OTA: nova versao disponivel: %s (atual %s) - tentativa %d/%d\n",
                novaVersao.c_str(), VERSAO_FW, otaTentativas, OTA_MAX_TENTATIVAS);
  String urlBin = OTA_URL_BINARIO + "?cb=" + String(esp_random());
  Serial.printf("OTA: baixando %s\n", urlBin.c_str());
  WiFiClientSecure clientUpdate; clientUpdate.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(clientUpdate, urlBin);
  if (ret == HTTP_UPDATE_FAILED)
    Serial.printf("OTA: FALHOU (%d): %s\n",
      httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
}

// ====== Sensor de solo: dois modelos ======
// O lote que chegou em set/26 fala 4800 e usa o bloco 0x0000; o que esta em
// campo fala 9600 e usa 0x0012. Em vez de escolher no build (e errar a caixa
// inteira), a estacao descobre sozinha no primeiro ciclo e guarda a resposta.
enum ModeloSolo : uint8_t { SOLO_INDEFINIDO = 0, SOLO_NOVO = 1, SOLO_VELHO = 2 };
RTC_DATA_ATTR ModeloSolo modeloSolo = SOLO_INDEFINIDO;

static void baud(uint32_t bps) {
  Serial2.updateBaudRate(bps);
  node.begin(1, Serial2);
  delay(50);
}

// LOTE NOVO: 4800, tudo num bloco so
static bool lerSoloNovo() {
  baud(4800);
  if (node.readHoldingRegisters(0x0000, 7) != node.ku8MBSuccess) return false;
  umidadeSolo   = node.getResponseBuffer(0) / 10.0;
  tempSolo      = node.getResponseBuffer(1) / 10.0;
  condutividade = node.getResponseBuffer(2);
  phSolo        = node.getResponseBuffer(3) / 10.0;
  nitrogenio    = node.getResponseBuffer(4);
  fosforo       = node.getResponseBuffer(5);
  potassio      = node.getResponseBuffer(6);
  return true;
}

// LOTE VELHO: 9600, quatro leituras separadas
static bool lerSoloVelho() {
  baud(9600);
  bool respondeu = false;
  if (node.readHoldingRegisters(0x0012, 2) == node.ku8MBSuccess) {
    umidadeSolo = node.getResponseBuffer(0) / 10.0;
    tempSolo    = node.getResponseBuffer(1) / 10.0;
    respondeu = true;
  }
  if (node.readHoldingRegisters(0x0015, 1) == node.ku8MBSuccess) {
    condutividade = node.getResponseBuffer(0);
    respondeu = true;
  }
  if (node.readHoldingRegisters(0x0007, 1) == node.ku8MBSuccess) {
    int leituraPH = node.getResponseBuffer(0);       // formula calibrada do V44
    phSolo = 5.5 + ((leituraPH - 2432.0) * 3.0) / (2666.0 - 2432.0);
    phSolo = constrain(phSolo, 3.0, 10.0);
    respondeu = true;
  }
  if (node.readHoldingRegisters(0x001E, 3) == node.ku8MBSuccess) {
    nitrogenio = node.getResponseBuffer(0);
    fosforo    = node.getResponseBuffer(1);
    potassio   = node.getResponseBuffer(2);
    respondeu = true;
  }
  return respondeu;
}

static void lerSolo() {
  if (modeloSolo == SOLO_NOVO  && lerSoloNovo())  return;
  if (modeloSolo == SOLO_VELHO && lerSoloVelho()) return;

  // indefinido, ou o modelo conhecido parou de responder: descobre de novo
  if (lerSoloNovo()) {
    if (modeloSolo != SOLO_NOVO) Serial.println("[solo] modelo NOVO (4800, bloco 0x0000)");
    modeloSolo = SOLO_NOVO;
  } else if (lerSoloVelho()) {
    if (modeloSolo != SOLO_VELHO) Serial.println("[solo] modelo VELHO (9600, 0x0012)");
    modeloSolo = SOLO_VELHO;
  } else {
    Serial.println("[solo] nenhum modelo respondeu — sensor desligado, sem cabo "
                   "ou rele nao armou (na bancada so USB o 7x1 nao tem 12V)");
    modeloSolo = SOLO_INDEFINIDO;
  }
}

void lerSensores() {
  analogSetAttenuation(ADC_11db);
  voltagemBateria = analogRead(VOLTIMETRO_PIN);
  temperaturaAr   = dht.readTemperature();
  umidadeAr       = dht.readHumidity();
  if (isnan(temperaturaAr)) temperaturaAr = 0;
  if (isnan(umidadeAr))     umidadeAr     = 0;

  lerSolo();
  noInterrupts(); pluviometroPulsosLidos = pluviometroPulsos; interrupts();

  Serial.printf("[LIDO] ar=%.1fC/%.1f%%  solo=%.1f%%/%.1fC EC=%.0f pH=%.2f NPK=%d/%d/%d bat=%.0f pluv=%u\n",
    temperaturaAr, umidadeAr, umidadeSolo, tempSolo, condutividade, phSolo,
    nitrogenio, fosforo, potassio, voltagemBateria, pluviometroPulsosLidos);
}

bool postParaSupabase(int indice) {
  StaticJsonDocument<512> doc;
  doc["dispositivo_id"]      = meuId;
  doc["versao_fw"]           = VERSAO_FW;
  doc["ciclo"]               = ciclo;
  doc["pacote"]              = indice + 1;
  doc["sensor_pos"]          = 1;
  doc["modelo_solo"]         = (modeloSolo == SOLO_NOVO)  ? "novo"
                             : (modeloSolo == SOLO_VELHO) ? "velho" : "mudo";
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

void dormir(long segundos) {
  Serial.printf("Dormindo %lds...\n", segundos);
  digitalWrite(RELE_PIN, HIGH);   // com o rele ligado antes do WiFi, todo
                                  // caminho de saida tem que desligar o sensor
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  led(false); digitalWrite(VEXT_PIN, HIGH);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUVIOMETRO_PIN, 0);
  esp_sleep_enable_timer_wakeup((uint64_t)segundos * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW); delay(1000);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, HIGH);   // rele desligado
  pinMode(RS485_DE_RE, OUTPUT); postTransmission();
  if (LED_STATUS != 255) { pinMode(LED_STATUS, OUTPUT); led(false); }
  dht.begin();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.begin(1, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  pinMode(PLUVIOMETRO_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PLUVIOMETRO_PIN), pluviometroISR, FALLING);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PLUVIOMETRO_PIN, 0);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0)      pluviometroPulsos++;
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) pluviometroPulsos = 0;

  ciclo++;
  Serial.printf("\n=== %s V%s | Ciclo %u | wakeup=%d ===\n",
    DEVICE_CODIGO, VERSAO_FW, ciclo, (int)wakeup_reason);

  // ---------------------------------------------------------------- WiFi
  Provisao::Config pcfg;
  pcfg.senhaAP           = "aiagro123";      // vai impressa na etiqueta
  pcfg.pinoLed           = LED_STATUS;       // 255 = sem LED (padrao do lote)
  pcfg.pinoBotao         = BOTAO_CONFIG;
  pcfg.timeoutPortalSeg  = 900;              // 15 min de portal no ar
  pcfg.dormirNoTimeout   = true;             // ninguem apareceu -> poupa bateria
  pcfg.sleepNoTimeoutSeg = 1800;             // e volta a chamar em 30 min

  // O botao e' checado AQUI, nao no loop(): o setup termina em deep sleep e o
  // loop() nunca chega a rodar. Isso cobre a janela em que a placa esta
  // acordada. Pra responder com a estacao dormindo faltaria wake por EXT1 —
  // ver LEIA-ME, secao do botao.
  Provisao::checarBotao(pcfg);

  String ap = Provisao::nomeAP(pcfg);
  bool bootFrio  = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED);
  bool pedePortal = false;

  if (bootFrio) {
    // liga/desliga 3x seguidas = pedido de reconfiguracao
    pedePortal = Provisao::pedidoPortalPorPowerCycle(3);
    if (pedePortal) Serial.println("[wifi] 3 power-cycles: abrindo portal a pedido");
  }
  if (!Provisao::temCredencial()) {
    Serial.println("[wifi] nenhuma rede salva — primeira configuracao");
    pedePortal = true;
  }

  bool online = false;
  if (pedePortal) {
    // o portal fica ate 15 min no ar esperando o produtor: sensor desligado
    digitalWrite(RELE_PIN, HIGH);
    mostrarPortal(ap, pcfg.senhaAP);
    if (Provisao::abrirPortal(pcfg)) {
      Serial.println("[wifi] configurado — reiniciando");
      mostrarStatus("WIFI OK!\nreiniciando...");
      delay(1500);
      ESP.restart();
    }
    // timeout do portal: normalmente dormiu la dentro (dormirNoTimeout).
    // Se algum dia esse flag mudar, garante que nao segue sem WiFi.
    dormir(1800);
  } else {
    mostrarStatus("CONECTANDO...");
    online = Provisao::conectar(pcfg);
    if (!online) {
      uint8_t falhas = incrementaFalhas();
      Serial.printf("[wifi] falha de conexao %u/3\n", falhas);
      if (falhas >= 3) {
        Serial.println("[wifi] 3 falhas seguidas — a rede pode ter mudado. Chamando pelo portal.");
        zeraFalhas();
        digitalWrite(RELE_PIN, HIGH);
        mostrarPortal(ap, pcfg.senhaAP);
        if (Provisao::abrirPortal(pcfg)) { mostrarStatus("WIFI OK!"); delay(1500); ESP.restart(); }
      }
      dormir(3600);   // tenta de novo daqui a 1h
    }
    zeraFalhas();
    Provisao::limparContadorBoot();   // ciclo engatou: nao era pedido de portal
  }

  // ---------------------------------------------------------------- identidade
  meuId = Provisao::deviceId();
  if (meuId.length() == 0) meuId = DISP_ID;
  if (meuId.length() == 0) {
    Serial.println("!! SEM UUID. Grave na bancada: mande \"id=<uuid>\" pelo serial.");
    mostrarStatus("SEM ID!\ngrave o UUID");
    Provisao::consoleSerial(60000);
    meuId = Provisao::deviceId();
    if (meuId.length()) ESP.restart();
    dormir(600);
  }
  Serial.printf("[id] dispositivo %s\n", meuId.c_str());

#ifdef MODO_TESTE
  Provisao::consoleSerial(5000);   // janela pra gravar UUID / conferir redes
#endif

  // ---------------------------------------------------------------- relogio
  struct tm timeinfo; bool horaOk = false;
  bool wakeTimer = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER);

  // Power-on e timer JA SAO a hora de enviar: o sono foi calculado pra acordar
  // exatamente no horario agendado. Nao precisa (nem deve) perguntar ao NTP —
  // era por isso que em campo nao funcionava: sem WiFi, horaOk ficava false,
  // ehHoraEnvio virava false e o ciclo inteiro (inclusive o rele) era pulado.
  bool deveEnviar = bootFrio || wakeTimer;

  // Virada do pluviometro e o unico caso que precisa consultar a hora, pra
  // saber se caiu bem no horario de envio ou se e so pra contar. E o unico
  // caso que NAO liga o rele: numa chuva sao dezenas de viradas, e cada uma
  // custaria 3 s de 7x1 energizado pra ler o mesmo solo.
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    if (online) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      if (getLocalTime(&timeinfo)) horaOk = true;
    }
    bool exatamenteHora = (horaOk &&
      timeinfo.tm_hour == HORA_ENVIO_AGENDADO && timeinfo.tm_min == MINUTO_ENVIO_AGENDADO);
    if (!exatamenteHora) {
      Serial.println("[PLUV] virada contada, voltando a dormir");
      dormir(60);
    }
    deveEnviar = true;
  }

  if (deveEnviar) {
    // ORDEM (b4 da Bela Vista, validada em campo): sensor PRIMEIRO, envio
    // depois. Leitura de solo nao pode depender de internet — e o clique do
    // rele e o unico sinal de vida que se ouve em campo, sem OLED.
    mostrarStatus("LIGANDO SENSOR");
    digitalWrite(RELE_PIN, LOW);   // liga sensor 7x1 (clique)
    delay(3000);                   // estabilizacao

    // Leitura de aquecimento DESCARTADA: a primeira resposta do 7x1 vem com
    // lixo. Sem ela, o primeiro envio de cada ciclo grava valor sujo no banco.
    Serial.println("--- Leitura de aquecimento (descartada) ---");
    lerSensores();
    delay(2000);

    for (int i = 0; i < NUMERO_DE_ENVIOS; i++) {
      mostrarStatus("LENDO " + String(i+1) + "/" + String(NUMERO_DE_ENVIOS));
      lerSensores();                 // le SEMPRE, com ou sem rede
      // reconexao limpa a cada envio: em sinal fraco a sessao as vezes fica
      // "presa" em CONNECTED sem transmitir (experiencia Bela/Olimpia)
      if (Provisao::conectar(pcfg)) {
        mostrarStatus("ENVIANDO " + String(i+1) + "/" + String(NUMERO_DE_ENVIOS) +
                      "\n" + Provisao::redeAtual());
        postParaSupabase(i);
      } else {
        Serial.printf("[envio] %d: sem WiFi, leitura feita mas nao enviada\n", i+1);
      }
      if (i < NUMERO_DE_ENVIOS - 1) delay(INTERVALO_ENVIO_MS);
    }
    digitalWrite(RELE_PIN, HIGH);   // desliga sensor
    mostrarStatus("CHECANDO OTA");
    verificarOTA();
  }

  // Hora pro calculo do sono: aproveita a conexao que os envios deixaram de pe.
  // Sem rede, cai no fallback de 1 h.
  if (!horaOk && WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    if (getLocalTime(&timeinfo)) horaOk = true;
  }

#ifdef MODO_TESTE
  dormir(SONO_TESTE_SEG);
#else
  long tempo_sono = 3600;
  if (horaOk) {
    long seg_hoje = (timeinfo.tm_hour * 3600L) + (timeinfo.tm_min * 60L) + timeinfo.tm_sec;
    long seg_obj  = (HORA_ENVIO_AGENDADO * 3600L) + (MINUTO_ENVIO_AGENDADO * 60L);
    tempo_sono = (seg_hoje < seg_obj) ? (seg_obj - seg_hoje) : (86400L - seg_hoje + seg_obj);
  }
  dormir(tempo_sono);
#endif
}

void loop() {}
