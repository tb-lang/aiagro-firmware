/* SCANNER MODBUS — com LOG explícito de relé + re-aplicação periódica.
 * Liga rele (LOW = liga) e mantem ligado o tempo todo, escaneia slaves 1-5
 * em registradores do LOTE VELHO.
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

void ligaRele() {
  digitalWrite(VEXT_PIN, LOW);
  digitalWrite(RELE_PIN, LOW);
  Serial.printf("[RELE] VEXT(gpio%d)=LOW, RELE(gpio%d)=LOW (ligado)\n", VEXT_PIN, RELE_PIN);
}

bool tenta(uint8_t slave, uint16_t reg, uint16_t qtd, const char* nome) {
  node.begin(slave, Serial2);
  while (Serial2.available()) Serial2.read();
  delay(200);
  for (int t = 1; t <= 3; t++) {
    uint8_t r = node.readHoldingRegisters(reg, qtd);
    if (r == node.ku8MBSuccess) {
      Serial.printf("  OK slave %d [%s @0x%04X x%d] tent %d:",
                    slave, nome, reg, qtd, t);
      for (int i = 0; i < qtd; i++) Serial.printf(" %u", node.getResponseBuffer(i));
      Serial.println();
      return true;
    }
    delay(300);
  }
  Serial.printf("  -- slave %d [%s @0x%04X] timeout 3x\n", slave, nome, reg);
  return false;
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SCANNER MODBUS COM RELE GARANTIDO ===");

  pinMode(VEXT_PIN, OUTPUT);
  pinMode(RELE_PIN, OUTPUT);
  ligaRele();
  pinMode(RS485_DE_RE, OUTPUT); postTx();
  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  node.preTransmission(preTx);
  node.postTransmission(postTx);

  Serial.println("Aguardando sensor estabilizar (5s)...");
  delay(5000);
}

void loop() {
  // Re-aplica rele (garante que nao caiu por brown-out / reset GPIO)
  ligaRele();
  Serial.printf("\n========= CICLO @%lus =========\n", millis()/1000);

  for (uint8_t s = 1; s <= 5; s++) {
    Serial.printf("--- Slave %d ---\n", s);
    bool achou = tenta(s, 0x0012, 2, "umid+temp velho");
    if (achou) {
      tenta(s, 0x0015, 1, "EC velho       ");
      tenta(s, 0x0006, 1, "pH velho       ");
      tenta(s, 0x001E, 3, "NPK velho      ");
    }
  }
  Serial.println("[10s ate proximo ciclo]");
  delay(10000);
}
