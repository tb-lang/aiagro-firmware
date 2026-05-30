/* SCANNER DOS 2 SLAVES (lote NOVO) — com calma.
 *
 * Lote NOVO: regs 0x0000-0x0006 numa unica leitura de 7 registradores
 *   [0]=TEMP (/10=C)  [1]=UMID (/10=%)  [2]=EC  [3]=pH (/100)
 *   [4]=N  [5]=P  [6]=K
 *
 * Loop:
 *   - le SLAVE 1 (com retries e flush antes)
 *   - respiro 5s
 *   - le SLAVE 2
 *   - respiro 10s, repete
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

void lerSlave(uint8_t slaveId) {
  Serial.printf("\n--- SLAVE %d (lote NOVO) ---\n", slaveId);
  while (Serial2.available()) Serial2.read();
  Serial2.flush();
  delay(300);
  node.begin(slaveId, Serial2);
  delay(100);

  for (int t = 1; t <= 5; t++) {
    while (Serial2.available()) Serial2.read();
    delay(200);
    uint8_t r = node.readHoldingRegisters(0x0000, 7);
    if (r == node.ku8MBSuccess) {
      uint16_t r0 = node.getResponseBuffer(0);
      uint16_t r1 = node.getResponseBuffer(1);
      uint16_t r2 = node.getResponseBuffer(2);
      uint16_t r3 = node.getResponseBuffer(3);
      uint16_t r4 = node.getResponseBuffer(4);
      uint16_t r5 = node.getResponseBuffer(5);
      uint16_t r6 = node.getResponseBuffer(6);
      Serial.printf("  OK (tent %d): raw [%u %u %u %u %u %u %u]\n",
                    t, r0, r1, r2, r3, r4, r5, r6);
      Serial.printf("  --> TEMP=%.1fC  UMID=%.1f%%  EC=%u  pH=%.2f  N=%u P=%u K=%u\n",
                    r0/10.0, r1/10.0, r2, r3/100.0, r4, r5, r6);
      return;
    } else {
      Serial.printf("  tent %d falhou (codigo %d)\n", t, r);
      delay(500);
    }
  }
  Serial.println("  >>> falhou apos 5 retries");
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SCANNER 2 SLAVES (lote NOVO) ===");

  pinMode(VEXT_PIN, OUTPUT); digitalWrite(VEXT_PIN, LOW);
  pinMode(RELE_PIN, OUTPUT); digitalWrite(RELE_PIN, LOW);
  pinMode(RS485_DE_RE, OUTPUT); postTx();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.preTransmission(preTx);
  node.postTransmission(postTx);

  Serial.println("RELE ON. Aguardando 3s pros sensores estabilizarem...");
  delay(3000);
}

void loop() {
  Serial.printf("\n========= CICLO @%lus =========\n", millis()/1000);
  lerSlave(1);
  Serial.println("\n[respiro 5s antes do slave 2]");
  delay(5000);
  lerSlave(2);
  Serial.println("\n[respiro 10s ate proximo ciclo]");
  delay(10000);
}
