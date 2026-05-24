/* ============================================================
 * MONITOR DE CALIBRACAO — sensor 7x1 RS485 (AiAgro)
 * ------------------------------------------------------------
 * Le os 7 registradores do sensor (0x0000..0x0006) em loop
 * rapido (~1.5s), SEM deep sleep / LoRa / WiFi.
 * Mostra umidade e EC em destaque pra acompanhar a calibracao
 * ao vivo (terra seca -> molhar -> ver reagir).
 *
 * Mapa lote novo:
 *   0x0000 umid(/10 %)  0x0001 temp(/10 C)  0x0002 EC(uS/cm)
 *   0x0003 pH(/100)     0x0004 N  0x0005 P  0x0006 K (mg/kg)
 *
 * Le slave 1 E slave 2.
 * Pinos placa AiAgro: RS485 TX 17 | RX 16 | DE/RE 32 | 9600 8N1
 * ============================================================ */
#include <Arduino.h>
#include <ModbusMaster.h>

#define RS485_TX     17
#define RS485_RX     16
#define RS485_DE_RE  32
#define SENSOR_BAUD  9600
#define RELE_PIN     26   // LOW = sensores ligados (igual a estacao)
#define VEXT_PIN      0   // LOW = sensores ligados

ModbusMaster node;
void preTransmission()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_RE, LOW); }

void setup() {
  Serial.begin(115200);
  delay(300);
  // LIGA alimentacao dos sensores (estado ativo = LOW)
  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, LOW);
  pinMode(RS485_DE_RE, OUTPUT);
  postTransmission();
  Serial2.begin(SENSOR_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  Serial.println();
  Serial.println("=== MONITOR CALIBRACAO 7x1 — umid e EC ao vivo (loop ~1.5s) ===");
  Serial.println("Ligando sensores, aguardando estabilizar (3s)...");
  delay(3000);
  // warmup
  node.begin(1, Serial2); node.readHoldingRegisters(0x0000,7); delay(400);
}

void lerSensor(uint8_t slave) {
  node.begin(slave, Serial2);
  uint8_t r = node.readHoldingRegisters(0x0000, 7);   // exatamente 7 regs
  if (r != node.ku8MBSuccess) {
    Serial.printf("  S%d: sem resposta (cod %d)\n", slave, r);
    return;
  }
  float umid = node.getResponseBuffer(0) / 10.0;
  float temp = node.getResponseBuffer(1) / 10.0;
  uint16_t ec = node.getResponseBuffer(2);
  float ph   = node.getResponseBuffer(3) / 100.0;
  uint16_t n = node.getResponseBuffer(4);
  uint16_t p = node.getResponseBuffer(5);
  uint16_t k = node.getResponseBuffer(6);
  Serial.printf("  S%d  >> UMID=%.1f%%  EC=%u  <<  | temp=%.1f pH=%.2f N=%u P=%u K=%u | raw[%u %u %u %u %u %u %u]\n",
                slave, umid, ec, temp, ph, n, p, k,
                node.getResponseBuffer(0), node.getResponseBuffer(1),
                node.getResponseBuffer(2), node.getResponseBuffer(3),
                node.getResponseBuffer(4), node.getResponseBuffer(5),
                node.getResponseBuffer(6));
}

void loop() {
  Serial.println("------------------------------------------------------------");
  lerSensor(1);
  delay(200);
  lerSensor(2);
  delay(1500);
}
