// =====================================================================
//  AiAgro — Provisionamento de WiFi pelo produtor (captive portal)
//  provisao.cpp  |  v1.0
// =====================================================================

#include "provisao.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_system.h>
#if __has_include(<esp_mac.h>)
  #include <esp_mac.h>
#endif

namespace {

// ---------------------------------------------------------------- estado
Preferences prefs;
WebServer   servidor(80);
DNSServer   dns;
const IPAddress IP_AP(192, 168, 4, 1);
const char*   NS = "aiagro_wifi";      // namespace da NVS (<= 15 chars)
const uint8_t MAX_REDES = 3;

enum Estado : uint8_t {
  OCIOSO = 0, TESTANDO, SUCESSO,
  ERRO_SENHA, ERRO_SEM_REDE, ERRO_SEM_INTERNET, ERRO_SINAL
};

Estado    estado        = OCIOSO;
String    ssidPendente, senhaPendente;
bool      pedidoTeste   = false;
bool      concluido     = false;
int8_t    rssiEscolhido = 0;
uint8_t   motivoWiFi    = 0;           // reason code do ultimo disconnect
String    cacheRedes    = "[]";
uint32_t  cacheRedesEm  = 0;
bool      ledLigado     = false;
Provisao::Config cfg;

void led(bool on) {
  ledLigado = on;
  if (cfg.pinoLed != 255) digitalWrite(cfg.pinoLed, on ? HIGH : LOW);
}

void onWiFiEvento(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    motivoWiFi = info.wifi_sta_disconnected.reason;
}

// ---------------------------------------------------------------- NVS
void lerRede(uint8_t i, String& ssid, String& senha) {
  prefs.begin(NS, true);
  ssid  = prefs.getString(("s" + String(i)).c_str(), "");
  senha = prefs.getString(("p" + String(i)).c_str(), "");
  prefs.end();
}

void salvarRede(const String& ssid, const String& senha) {
  // le o que ja existe, tira duplicata e coloca a nova na frente
  String s[MAX_REDES], p[MAX_REDES];
  for (uint8_t i = 0; i < MAX_REDES; i++) lerRede(i, s[i], p[i]);

  String ns[MAX_REDES], np[MAX_REDES];
  ns[0] = ssid; np[0] = senha;
  uint8_t k = 1;
  for (uint8_t i = 0; i < MAX_REDES && k < MAX_REDES; i++) {
    if (s[i].length() && s[i] != ssid) { ns[k] = s[i]; np[k] = p[i]; k++; }
  }

  prefs.begin(NS, false);
  for (uint8_t i = 0; i < MAX_REDES; i++) {
    prefs.putString(("s" + String(i)).c_str(), ns[i]);
    prefs.putString(("p" + String(i)).c_str(), np[i]);
  }
  prefs.end();
}

// ------------------------------------------------------------- utilidades
String escapaJson(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { /* descarta controle */ }
    else out += c;
  }
  return out;
}

// Varre e devolve JSON ja ordenado por sinal, sem SSID repetido.
String escanear() {
  if (millis() - cacheRedesEm < 8000 && cacheRedes.length() > 2) return cacheRedes;

  int n = WiFi.scanNetworks(false, true);   // sincrono, mostra ocultas
  String json = "[";
  bool primeiro = true;

  for (int rank = 0; rank < n && rank < 20; rank++) {
    int melhor = -1;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i).length() == 0) continue;
      if (json.indexOf("\"" + escapaJson(WiFi.SSID(i)) + "\"") >= 0) continue;  // ja impresso
      if (melhor < 0 || WiFi.RSSI(i) > WiFi.RSSI(melhor)) melhor = i;
    }
    if (melhor < 0) break;

    if (!primeiro) json += ",";
    primeiro = false;
    json += "{\"ssid\":\"" + escapaJson(WiFi.SSID(melhor)) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(melhor)) + ",";
    json += "\"aberta\":" + String(WiFi.encryptionType(melhor) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();

  cacheRedes   = json;
  cacheRedesEm = millis();
  return json;
}

bool temInternet() {
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(cfg.urlTesteInternet)) return false;
  int code = http.GET();
  http.end();
  return (code == 204 || code == 200 || code == 302);
}

// Associa e valida de verdade. Nao grava nada — quem grava e o chamador.
Estado testarRede(const String& ssid, const String& senha) {
  motivoWiFi = 0;
  WiFi.disconnect(false, true);
  delay(200);
  WiFi.begin(ssid.c_str(), senha.length() ? senha.c_str() : nullptr);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < cfg.timeoutConexaoMs) {
    delay(250);
    led((millis() / 250) % 2);
  }
  led(false);

  if (WiFi.status() != WL_CONNECTED) {
    // 201 NO_AP_FOUND | 202 AUTH_FAIL | 15 4WAY_HANDSHAKE_TIMEOUT
    Serial.printf("[provisao] falhou, reason=%u\n", motivoWiFi);
    if (motivoWiFi == 201) return ERRO_SEM_REDE;
    return ERRO_SENHA;
  }

  rssiEscolhido = WiFi.RSSI();
  if (rssiEscolhido < cfg.rssiMinimoAviso) { WiFi.disconnect(); return ERRO_SINAL; }
  if (!temInternet())                      { WiFi.disconnect(); return ERRO_SEM_INTERNET; }
  return SUCESSO;
}

// ---------------------------------------------------------------- pagina
const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="pt-BR"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AiAgro - conectar na internet</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
background:#F4F7F3;color:#14281D;padding-bottom:40px}
header{background:#0F5132;color:#fff;padding:18px 20px}
header b{font-size:20px;letter-spacing:.5px}
header span{display:block;font-size:13px;opacity:.8;margin-top:2px}
main{max-width:520px;margin:0 auto;padding:18px 16px}
.card{background:#fff;border:1px solid #E1E8E1;border-radius:14px;padding:16px;margin-bottom:14px}
h2{font-size:15px;text-transform:uppercase;letter-spacing:.6px;color:#4A6152;margin-bottom:10px}
.rede{display:flex;align-items:center;gap:10px;width:100%;background:none;border:0;
border-bottom:1px solid #EEF2EE;padding:13px 2px;text-align:left;font-size:16px;cursor:pointer;color:#14281D}
.rede:last-child{border-bottom:0}
.rede .nome{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bar{width:34px;height:8px;border-radius:4px;background:#E3E8E3;overflow:hidden;flex:none}
.bar i{display:block;height:100%}
.b3 i{background:#22A75A}.b2 i{background:#E9A812}.b1 i{background:#D94A3D}
.lock{font-size:12px;color:#7C8C81}
input{width:100%;padding:13px;border:1px solid #CBD6CD;border-radius:10px;font-size:16px;background:#fff}
label{display:block;font-size:13px;color:#4A6152;margin:12px 0 5px}
.btn{display:block;width:100%;padding:14px;border:0;border-radius:10px;background:#0F5132;
color:#fff;font-size:16px;font-weight:600;margin-top:14px;cursor:pointer}
.btn.sec{background:#fff;color:#0F5132;border:1px solid #0F5132}
.aviso{background:#FFF6E0;border:1px solid #F0DBA8;color:#6B5312;font-size:13px;
padding:10px 12px;border-radius:10px;margin-top:12px}
.erro{background:#FDECEA;border:1px solid #F3C3BD;color:#7E2318;font-size:14px;padding:12px;border-radius:10px}
.ok{background:#E8F6EE;border:1px solid #B4E0C6;color:#0F5132;font-size:14px;padding:12px;border-radius:10px}
.mini{font-size:13px;color:#7C8C81;margin-top:8px}
.link{background:none;border:0;color:#0F5132;text-decoration:underline;font-size:14px;padding:8px 0;cursor:pointer}
.hide{display:none}
.sp{width:22px;height:22px;border:3px solid #D8E4DB;border-top-color:#0F5132;border-radius:50%;
display:inline-block;vertical-align:-5px;margin-right:8px;animation:g 1s linear infinite}
@keyframes g{to{transform:rotate(360deg)}}
</style></head><body>
<header><b>AiAgro</b><span id="unid">estacao</span></header>
<main>

<div class="card" id="p1">
  <h2>1. Escolha a rede da fazenda</h2>
  <div id="lista"><p class="mini"><span class="sp"></span>procurando redes...</p></div>
  <button class="btn sec" onclick="carregar()">Procurar de novo</button>
  <button class="link" onclick="manual()">Minha rede nao aparece na lista</button>
  <div class="aviso">A estacao so enxerga redes de <b>2,4 GHz</b>. Se a sua rede
  aparece no celular e nao aqui, ela provavelmente esta em 5 GHz — peca pra separar
  as bandas no roteador ou ligue a rede de 2,4 GHz.</div>
</div>

<div class="card hide" id="p2">
  <h2>2. Senha do WiFi</h2>
  <div id="alvo" style="font-size:18px;font-weight:600"></div>
  <div class="mini" id="sinal"></div>
  <label for="pw">Senha</label>
  <input id="pw" type="password" autocomplete="off" autocapitalize="off" spellcheck="false">
  <label style="display:flex;align-items:center;gap:8px;margin-top:10px">
    <input type="checkbox" style="width:auto" onchange="document.getElementById('pw').type=this.checked?'text':'password'"> mostrar senha
  </label>
  <button class="btn" onclick="salvar()">Conectar</button>
  <button class="btn sec" onclick="volta()">Voltar</button>
</div>

<div class="card hide" id="p3">
  <h2>Testando</h2>
  <p><span class="sp"></span><span id="msg">conectando na rede...</span></p>
  <div class="aviso">Essa telinha pode travar ou cair por alguns segundos durante o
  teste — e normal. Se cair, reconecte no WiFi <b id="apn"></b> e abra de novo.</div>
</div>

<div class="card hide" id="p4"><div id="res"></div>
  <button class="btn sec" id="volta4" onclick="volta()">Tentar outra rede</button></div>

</main><script>
var sel="", manualOn=false;
function $(i){return document.getElementById(i)}
fetch('/info').then(function(r){return r.json()}).then(function(d){
  $('unid').textContent=d.ap+"  ·  "+d.mac;$('apn').textContent=d.ap});
function barra(r){var n=r>=-67?3:(r>=-80?2:1);return '<span class="bar b'+n+'"><i style="width:'+
  Math.max(15,Math.min(100,2*(r+100)))+'%"></i></span>'}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;')}
function carregar(){
  $('lista').innerHTML='<p class="mini"><span class="sp"></span>procurando redes...</p>';
  fetch('/redes').then(function(r){return r.json()}).then(function(rs){
    if(!rs.length){$('lista').innerHTML='<p class="mini">Nenhuma rede 2,4 GHz encontrada aqui.</p>';return}
    $('lista').innerHTML=rs.map(function(r,i){return '<button class="rede" onclick="pick('+i+')">'+
      barra(r.rssi)+'<span class="nome">'+esc(r.ssid)+'</span><span class="lock">'+
      (r.aberta?'aberta':'&#128274;')+'</span></button>'}).join('');
    window.RS=rs;
  }).catch(function(){setTimeout(carregar,1500)})
}
function pick(i){
  var r=window.RS[i];sel=r.ssid;manualOn=false;
  $('alvo').textContent=sel;
  $('sinal').innerHTML= r.rssi>=-70 ? 'Sinal bom ('+r.rssi+' dBm)'
    : (r.rssi>=-80 ? 'Sinal fraco ('+r.rssi+' dBm) - pode falhar em dia de chuva. Vale um repetidor.'
                   : 'Sinal muito fraco ('+r.rssi+' dBm) - a estacao nao vai aguentar assim.');
  $('pw').value='';$('pw').disabled=!!r.aberta;
  $('p1').classList.add('hide');$('p2').classList.remove('hide');
}
function manual(){
  manualOn=true;
  $('alvo').innerHTML='<input id="ms" placeholder="nome exato da rede" autocapitalize="off">';
  $('sinal').textContent='Digite o nome exatamente como esta no roteador (maiusculas contam).';
  $('pw').disabled=false;$('pw').value='';
  $('p1').classList.add('hide');$('p2').classList.remove('hide');
}
function volta(){['p2','p3','p4'].forEach(function(i){$(i).classList.add('hide')});
  $('p1').classList.remove('hide');carregar()}
function salvar(){
  var ssid = manualOn ? ($('ms').value||'') : sel;
  if(!ssid){alert('Escolha ou digite a rede');return}
  $('p2').classList.add('hide');$('p3').classList.remove('hide');
  fetch('/salvar',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&senha='+encodeURIComponent($('pw').value)});
  setTimeout(poll,2500);
}
var falhas=0;
function poll(){
  fetch('/estado').then(function(r){return r.json()}).then(function(d){
    falhas=0;
    if(d.e==1){$('msg').textContent=d.t;setTimeout(poll,1500);return}
    if(d.e==0){setTimeout(poll,1500);return}
    $('p3').classList.add('hide');$('p4').classList.remove('hide');
    if(d.e==2){$('res').innerHTML='<div class="ok"><b>Conectado!</b><br>'+d.t+
      '</div><p class="mini">Pode fechar essa tela. A estacao vai reiniciar sozinha e '+
      'ja comeca a enviar as leituras.</p>';$('volta4').classList.add('hide')}
    else{$('res').innerHTML='<div class="erro"><b>Nao deu certo.</b><br>'+d.t+'</div>'}
  }).catch(function(){falhas++;setTimeout(poll, falhas>6?4000:1500)})
}
carregar();
</script></body></html>)HTML";

// --------------------------------------------------------------- handlers
void hRaiz()  { servidor.send_P(200, "text/html", PORTAL_HTML); }
void hRedes() { servidor.send(200, "application/json", escanear()); }

void hInfo() {
  servidor.send(200, "application/json",
    "{\"ap\":\"" + Provisao::nomeAP(cfg) + "\",\"mac\":\"" + WiFi.macAddress() + "\"}");
}

void hSalvar() {
  ssidPendente  = servidor.arg("ssid");
  senhaPendente = servidor.arg("senha");
  // Limites do proprio padrao WiFi: SSID 32 bytes, senha WPA2 63. Sem isso,
  // uma entrada anormalmente grande ia parar na NVS e no WiFi.begin().
  if (ssidPendente.length() == 0 || ssidPendente.length() > 32 ||
      senhaPendente.length() > 63) {
    Serial.printf("[provisao] recusado: ssid=%u senha=%u bytes\n",
                  ssidPendente.length(), senhaPendente.length());
    servidor.send(400, "text/plain", "ssid ou senha fora do tamanho");
    return;
  }
  estado      = TESTANDO;
  pedidoTeste = true;
  servidor.send(200, "application/json", "{\"ok\":true}");
}

void hEstado() {
  const char* t;
  switch (estado) {
    case TESTANDO:          t = "conectando na rede..."; break;
    case SUCESSO:           t = "Rede salva. Sinal e internet confirmados."; break;
    case ERRO_SENHA:        t = "A rede respondeu mas recusou a senha. Confira maiusculas, "
                                "espacos e o ponto/hifen no fim."; break;
    case ERRO_SEM_REDE:     t = "Nao achei essa rede daqui. A estacao pode estar longe demais "
                                "do roteador, ou a rede e 5 GHz."; break;
    case ERRO_SEM_INTERNET: t = "Conectou no roteador mas nao chegou na internet. Cheque se o "
                                "provedor esta no ar ou se a rede exige login/liberacao."; break;
    case ERRO_SINAL:        t = "Conectou, mas o sinal aqui e fraco demais pra aguentar o dia a "
                                "dia. Aproxime a estacao ou instale um repetidor."; break;
    default:                t = ""; break;
  }
  servidor.send(200, "application/json",
    "{\"e\":" + String((int)estado) + ",\"t\":\"" + String(t) + "\"}");
}

// Qualquer outra URL vira redirect pro portal — e isso que faz o celular
// abrir a telinha sozinho (iOS /hotspot-detect.html, Android /generate_204).
void hNaoAchou() {
  servidor.sendHeader("Location", "http://192.168.4.1/", true);
  servidor.send(302, "text/plain", "");
}

} // namespace anonimo

// ===================================================================== API
namespace Provisao {

String nomeAP(const Config& c) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suf[6];
  snprintf(suf, sizeof(suf), "%02X%02X", mac[4], mac[5]);
  return String(c.prefixoAP) + "-" + suf;
}

bool temCredencial() {
  String s, p;
  for (uint8_t i = 0; i < MAX_REDES; i++) { lerRede(i, s, p); if (s.length()) return true; }
  return false;
}

void limpar() {
  prefs.begin(NS, false);
  for (uint8_t i = 0; i < MAX_REDES; i++) {
    prefs.remove(("s" + String(i)).c_str());
    prefs.remove(("p" + String(i)).c_str());
  }
  // Zera junto a contagem de power-cycle: sem isso podia sobrar contagem
  // residual (ex. 2/3) e a estacao reabrir o portal logo depois de configurar.
  prefs.putUChar("boots", 0);
  prefs.end();
}

String redeAtual() { return (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String(""); }

String deviceId() {
  prefs.begin(NS, true);
  String id = prefs.getString("uuid", "");
  prefs.end();
  return id;
}

void gravarDeviceId(const String& uuid) {
  prefs.begin(NS, false);
  prefs.putString("uuid", uuid);
  prefs.end();
}

bool pedidoPortalPorPowerCycle(uint8_t vezes) {
  prefs.begin(NS, false);
  uint8_t n = prefs.getUChar("boots", 0) + 1;
  prefs.putUChar("boots", n);
  prefs.end();
  Serial.printf("[provisao] power-cycles seguidos: %u/%u\n", n, vezes);
  if (n >= vezes) {
    prefs.begin(NS, false); prefs.putUChar("boots", 0); prefs.end();
    return true;
  }
  return false;
}

void limparContadorBoot() {
  prefs.begin(NS, false);
  if (prefs.getUChar("boots", 0) != 0) prefs.putUChar("boots", 0);
  prefs.end();
}

bool conectar(const Config& c) {
  cfg = c;
  if (cfg.pinoLed   != 255) pinMode(cfg.pinoLed, OUTPUT);
  if (cfg.pinoBotao != 255) pinMode(cfg.pinoBotao, INPUT_PULLUP);

  WiFi.onEvent(onWiFiEvento);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (uint8_t i = 0; i < MAX_REDES; i++) {
    String ssid, senha;
    lerRede(i, ssid, senha);
    if (!ssid.length()) continue;

    for (uint8_t t = 0; t < cfg.tentativasPorRede; t++) {
      Serial.printf("[provisao] tentando \"%s\" (%u/%u)\n", ssid.c_str(), t + 1, cfg.tentativasPorRede);
      motivoWiFi = 0;
      WiFi.disconnect(false, true);
      delay(200);
      WiFi.begin(ssid.c_str(), senha.length() ? senha.c_str() : nullptr);
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < cfg.timeoutConexaoMs) delay(250);
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[provisao] conectado em %s (%d dBm), IP %s\n",
                      ssid.c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
        led(true);
        return true;
      }
    }
  }
  Serial.println("[provisao] nenhuma rede salva entrou");
  return false;
}

bool abrirPortal(const Config& c) {
  cfg       = c;
  estado    = OCIOSO;
  concluido = false;
  if (cfg.pinoLed   != 255) pinMode(cfg.pinoLed, OUTPUT);
  if (cfg.pinoBotao != 255) pinMode(cfg.pinoBotao, INPUT_PULLUP);

  WiFi.onEvent(onWiFiEvento);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  String ap = nomeAP(cfg);
  bool comSenha = cfg.senhaAP && strlen(cfg.senhaAP) >= 8;
  WiFi.softAP(ap.c_str(), comSenha ? cfg.senhaAP : nullptr, cfg.canalAP);
  delay(400);
  WiFi.softAPConfig(IP_AP, IP_AP, IPAddress(255, 255, 255, 0));

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", IP_AP);

  servidor.on("/",       HTTP_GET,  hRaiz);
  servidor.on("/redes",  HTTP_GET,  hRedes);
  servidor.on("/info",   HTTP_GET,  hInfo);
  servidor.on("/estado", HTTP_GET,  hEstado);
  servidor.on("/salvar", HTTP_POST, hSalvar);
  servidor.onNotFound(hNaoAchou);
  servidor.begin();

  Serial.printf("[provisao] PORTAL NO AR — rede \"%s\" senha \"%s\" -> http://192.168.4.1\n",
                ap.c_str(), comSenha ? cfg.senhaAP : "(aberta)");

  uint32_t t0 = millis();
  uint32_t piscaEm = 0;

  while (!concluido) {
    dns.processNextRequest();
    servidor.handleClient();

    if (millis() - piscaEm > 1200) { piscaEm = millis(); led(!ledLigado); }

    if (pedidoTeste) {
      pedidoTeste = false;
      Serial.printf("[provisao] testando \"%s\"\n", ssidPendente.c_str());
      Estado r = testarRede(ssidPendente, senhaPendente);
      estado = r;
      if (r == SUCESSO) {
        salvarRede(ssidPendente, senhaPendente);
        Serial.printf("[provisao] OK, salvo. sinal %d dBm\n", rssiEscolhido);
        // 6s pro celular ver a tela de sucesso antes de derrubar o AP
        uint32_t f = millis();
        while (millis() - f < 6000) { dns.processNextRequest(); servidor.handleClient(); delay(5); }
        concluido = true;
      }
      t0 = millis();   // interacao zera o timeout do portal
    }

    if (cfg.timeoutPortalSeg && (millis() - t0) / 1000 > cfg.timeoutPortalSeg) {
      Serial.println("[provisao] timeout do portal");
      servidor.stop(); dns.stop(); WiFi.softAPdisconnect(true);
      if (cfg.dormirNoTimeout) {
        esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepNoTimeoutSeg * 1000000ULL);
        esp_deep_sleep_start();
      }
      return false;
    }
    delay(2);
  }

  servidor.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  led(true);
  return true;
}

void checarBotao(const Config& c) {
  if (c.pinoBotao == 255) return;
  if (digitalRead(c.pinoBotao) != LOW) return;

  uint32_t t0 = millis();
  while (digitalRead(c.pinoBotao) == LOW) {
    if (millis() - t0 > 5000) {
      Serial.println("[provisao] reset de fabrica — apagando redes salvas");
      for (uint8_t i = 0; i < 6; i++) { led(true); delay(120); led(false); delay(120); }
      limpar();
      delay(300);
      ESP.restart();
    }
    delay(50);
  }
}

void consoleSerial(uint32_t janelaMs) {
  Serial.printf("\n[bancada] console aberto por %us — comandos: id=<uuid> | wifi? | reset\n",
                janelaMs / 1000);
  uint32_t t0 = millis();
  String buf;
  while (millis() - t0 < janelaMs) {
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch == '\n' || ch == '\r') {
        buf.trim();
        if (buf.startsWith("id=")) {
          String uuid = buf.substring(3); uuid.trim();
          gravarDeviceId(uuid);
          Serial.printf("[bancada] UUID gravado: %s\n", uuid.c_str());
        } else if (buf == "wifi?") {
          String s, p;
          for (uint8_t i = 0; i < MAX_REDES; i++) {
            lerRede(i, s, p);
            if (s.length()) Serial.printf("[bancada] rede %u: %s\n", i, s.c_str());
          }
          Serial.printf("[bancada] uuid: %s\n", deviceId().c_str());
        } else if (buf == "reset") {
          limpar();
          Serial.println("[bancada] redes apagadas");
        } else if (buf.length()) {
          Serial.println("[bancada] comando desconhecido");
        }
        buf = "";
        t0 = millis();   // digitou: estende a janela
      } else if (buf.length() < 80) {
        buf += ch;
      }
    }
    delay(10);
  }
  Serial.println("[bancada] console fechado");
}

} // namespace Provisao
