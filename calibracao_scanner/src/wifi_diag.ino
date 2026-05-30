/* CALIBRACAO UMIDADE — slave 1 lote velho, le a cada 3s, mostra raw.
 *
 * Procedimento:
 *   1. Sensor seco no AR → anota raw min
 *   2. Sensor mergulhado em AGUA → anota raw max
 *   3. Calcula: pct_real = (raw - raw_min) * 100 / (raw_max - raw_min)
 */
#include <Arduino.h>
#include <ModbusMaster.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define RS485_TX    17
#define RS485_RX    16
#define RS485_DE_RE 32
#define RELE_PIN    26
#define VEXT_PIN     0

ModbusMaster node;
void preTx()  { digitalWrite(RS485_DE_RE, HIGH); }
void postTx() { digitalWrite(RS485_DE_RE, LOW); }

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== CALIBRACAO UMIDADE (slave 1 lote velho) ===");
  Serial.println("Le a cada 3s e mostra raw + conversao /10.");
  Serial.println("Procedimento: deixa no AR um tempo, depois mergulha em AGUA.\n");

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, LOW);
  pinMode(RS485_DE_RE, OUTPUT); postTx();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.begin(1, Serial2);
  node.preTransmission(preTx);
  node.postTransmission(postTx);

  delay(3000);
}

void loop() {
  // 5 retries
  uint8_t r = 0xFF;
  uint16_t raw_u = 0, raw_t = 0;
  for (int t = 1; t <= 5; t++) {
    while (Serial2.available()) Serial2.read();
    delay(200);
    r = node.readHoldingRegisters(0x0012, 2);
    if (r == node.ku8MBSuccess) {
      raw_u = node.getResponseBuffer(0);
      raw_t = node.getResponseBuffer(1);
      break;
    }
    delay(300);
  }
  if (r == node.ku8MBSuccess) {
    Serial.printf("[%6lus]  raw_umid=%4u (umid=%.1f%%)   raw_temp=%4u (temp=%.1fC)\n",
                  millis()/1000, raw_u, raw_u/10.0, raw_t, raw_t/10.0);
  } else {
    Serial.printf("[%6lus]  ERRO leitura (codigo %d)\n", millis()/1000, r);
  }
  delay(3000);
}
