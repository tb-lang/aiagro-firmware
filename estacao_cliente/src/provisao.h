// =====================================================================
//  AiAgro — Provisionamento de WiFi pelo produtor (captive portal)
//  provisao.h  |  v1.0
// ---------------------------------------------------------------------
//  A estacao sobe um WiFi proprio ("AiAgro-3F2A"). O produtor conecta o
//  celular nesse WiFi, a telinha abre sozinha (igual portal de hotel),
//  ele escolhe a rede da fazenda, digita a senha, a estacao TESTA ao
//  vivo (associa + sinal + internet de verdade) e so entao grava na NVS.
//
//  Guarda ate 3 redes. No boot tenta as salvas em ordem.
//
//  Como o produtor pede o portal em campo (sem notebook, sem tecnico):
//    a) liga e desliga a estacao 3x seguidas (cada vez ~5s ligada); ou
//    b) segura o botao CONFIG (opcional, GPIO33 -> GND) por 5s; ou
//    c) automatico: nenhuma rede salva funciona.
//
//  Dependencias: so o core do ESP32. Nenhuma lib externa.
// =====================================================================

#pragma once
#include <Arduino.h>

namespace Provisao {

struct Config {
  // --- AP de configuracao -------------------------------------------
  const char* prefixoAP = "AiAgro";      // vira "AiAgro-3F2A" (4 do MAC)
  const char* senhaAP   = "aiagro123";   // >= 8 chars; "" deixa aberto
  uint8_t     canalAP   = 1;

  // --- Tentativa de conexao -----------------------------------------
  uint32_t timeoutConexaoMs   = 15000;   // por tentativa
  uint8_t  tentativasPorRede  = 2;

  // --- Portal --------------------------------------------------------
  uint32_t timeoutPortalSeg   = 900;     // 15 min sem ninguem -> desiste
  bool     dormirNoTimeout    = false;   // true = deep sleep (estacao a bateria)
  uint32_t sleepNoTimeoutSeg  = 1800;    // e acorda em 30 min pra tentar de novo

  // --- Qualidade de sinal --------------------------------------------
  int8_t rssiMinimoOk    = -70;          // acima disso: verde
  int8_t rssiMinimoAviso = -80;          // abaixo disso: reprova com recado

  // --- Hardware -------------------------------------------------------
  uint8_t pinoBotao = 255;               // 255 = sem botao (GPIO33 se soldar)
  uint8_t pinoLed   = 255;               // 255 = sem LED de status

  // --- Teste de internet ----------------------------------------------
  // HTTP puro de proposito: TLS exige relogio certo, e no primeiro boot
  // o relogio ainda nao sincronizou (foi a raiz do "sem foto" da Laranja).
  const char* urlTesteInternet = "http://connectivitycheck.gstatic.com/generate_204";
};

// Tenta as redes salvas na NVS. true = associado (nao testa internet).
bool conectar(const Config& cfg = Config());

// Sobe o AP + captive portal e BLOQUEIA ate o produtor configurar.
// true = configurou e validou. false = timeout (ou dormiu, se configurado).
bool abrirPortal(const Config& cfg = Config());

// Ha pelo menos uma rede gravada?
bool temCredencial();

// Apaga todas as redes salvas (mantem o UUID do dispositivo).
void limpar();

// Nome do AP desta unidade — o mesmo que vai na etiqueta/QR.
String nomeAP(const Config& cfg = Config());

// SSID da rede em que esta conectado agora (vazio se nenhuma).
String redeAtual();

// --- Gatilhos de campo -------------------------------------------------
// Chamar UMA vez no inicio do setup(): conta power-cycles rapidos.
// true = o produtor ligou/desligou 3x => abrir o portal.
bool pedidoPortalPorPowerCycle(uint8_t vezes = 3);

// Chamar depois que o ciclo "engatou" (ja conectou/leu). Zera o contador
// pra que um desligamento normal amanha nao conte como pedido de portal.
void limparContadorBoot();

// Botao fisico opcional: segurado 5s = apaga redes e reinicia no portal.
void checarBotao(const Config& cfg = Config());

// --- Identidade da unidade ---------------------------------------------
// UUID gravado na bancada antes de embalar (mesmo do QR da etiqueta).
// Vazio = cai no DISP_ID compilado.
String deviceId();
void   gravarDeviceId(const String& uuid);

// Console de bancada pelo serial: aceita "id=<uuid>", "wifi?" e "reset".
// Chamar no setup durante alguns segundos, so em firmware de teste.
void consoleSerial(uint32_t janelaMs);

} // namespace Provisao
