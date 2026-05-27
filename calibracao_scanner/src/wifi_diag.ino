/* SCANNER REGISTRADORES 7x1 (lote velho) — acha o pH e mapeia tudo.
 * Le slave 1, registradores 0x0000..0x0020 um a um, mostra os que respondem.
 * Pinos: RS485 TX17 RX16 DE/RE32 | VEXT0 RELE26 (LOW=liga sensor) | 9600 8N1 */
#include <Arduino.h>
#include <ModbusMaster.h>
#define RS485_TX 17
#define RS485_RX 16
#define RS485_DE_RE 32
#define VEXT_PIN 0
#define RELE_PIN 26
ModbusMaster node;
void pre(){digitalWrite(RS485_DE_RE,HIGH);} void post(){digitalWrite(RS485_DE_RE,LOW);}
void setup(){
  Serial.begin(115200); delay(400);
  pinMode(VEXT_PIN,OUTPUT); digitalWrite(VEXT_PIN,LOW);
  pinMode(RELE_PIN,OUTPUT); digitalWrite(RELE_PIN,LOW);
  pinMode(RS485_DE_RE,OUTPUT); post();
  Serial2.begin(9600,SERIAL_8N1,RS485_RX,RS485_TX);
  node.begin(1,Serial2); node.preTransmission(pre); node.postTransmission(post);
  Serial.println("\n=== SCAN REGISTRADORES slave 1 (lote velho) — liga 3s ===");
  delay(3000);
  node.readHoldingRegisters(0x0012,1); delay(300); // warmup
}
void loop(){
  Serial.println("---- registradores que respondem (reg: valor / /10 / /100) ----");
  for(uint16_t reg=0x0000; reg<=0x0020; reg++){
    if(node.readHoldingRegisters(reg,1)==node.ku8MBSuccess){
      uint16_t v=node.getResponseBuffer(0);
      Serial.printf("  0x%04X = %5u   (/10=%.1f  /100=%.2f)\n", reg, v, v/10.0, v/100.0);
    }
    delay(40);
  }
  Serial.println("(repete em 5s)\n");
  delay(5000);
}
