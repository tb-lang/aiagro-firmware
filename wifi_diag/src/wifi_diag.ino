/* TESTE RURAL — insiste no WiFi (varias tentativas, paciencia) e quando conecta,
 * manda pacote SIMULADO pro Supabase. Nao desiste: fica tentando ate conseguir.
 * Linhas marcadas versao_fw="SIGTEST" (apagar depois). */
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define WIFI_SSID "MECSEGUR2"
#define WIFI_PASS "12345678"
#define TENTATIVAS_WIFI   15      // ate 15 tentativas de conexao
#define TIMEOUT_POR_TENT  25000   // 25s por tentativa
#define ESPERA_ENTRE      5000    // 5s entre tentativas

const char* URL  = "https://bwtotmprzmldczafjhrg.supabase.co/rest/v1/leituras";
const char* ANON = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJ3dG90bXByem1sZGN6YWZqaHJnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzgwNzI0MjAsImV4cCI6MjA5MzY0ODQyMH0.-ZiY9JSdsUoCC2dSsesYemH-vN61Gl5odX9XrRxc-jo";
const char* DISP_EST = "a354757c-6a63-4a02-9ec8-554ca8004c64";
const char* DISP_REC = "1473ecf6-9ec7-4708-a784-3057dcf98d76";

int pct(int rssi){ int p=2*(rssi+100); if(p>100)p=100; if(p<0)p=0; return p; }

bool conectarWiFi(){
  for(int tent=1; tent<=TENTATIVAS_WIFI; tent++){
    Serial.printf("[WiFi] tentativa %d/%d ", tent, TENTATIVAS_WIFI);
    WiFi.disconnect(true); delay(300);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t<TIMEOUT_POR_TENT){ delay(500); Serial.print("."); }
    if(WiFi.status()==WL_CONNECTED){
      Serial.printf("\n[WiFi] CONECTOU na tentativa %d | IP=%s | sinal=%d%% (%d dBm)\n",
        tent, WiFi.localIP().toString().c_str(), pct(WiFi.RSSI()), WiFi.RSSI());
      return true;
    }
    Serial.printf(" falhou. esperando %ds...\n", ESPERA_ENTRE/1000);
    delay(ESPERA_ENTRE);
  }
  Serial.println("[WiFi] nao conseguiu apos todas as tentativas.");
  return false;
}

bool enviarSupabase(){
  int rssi=WiFi.RSSI(), p=pct(rssi);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setConnectTimeout(12000); http.setTimeout(15000);
  http.begin(client, URL);
  http.addHeader("apikey", ANON);
  http.addHeader("Authorization", String("Bearer ")+ANON);
  http.addHeader("Content-Type","application/json");
  String body = String("{\"dispositivo_id\":\"")+DISP_EST+"\",\"receptora_id\":\""+DISP_REC+
                "\",\"versao_fw\":\"SIGTEST\",\"sensor_pos\":9,\"sinal_wifi_pct\":"+p+
                ",\"umid_solo\":0,\"temp_solo\":0,\"ec\":0}";
  unsigned long t0=millis();
  int code=http.POST(body);
  unsigned long dt=millis()-t0;
  http.end();
  if(code==201){ Serial.printf(">> ENVIOU OK (201) em %lums | sinal=%d%% (%d dBm)\n", dt,p,rssi); return true; }
  Serial.printf(">> envio FALHOU (HTTP %d) em %lums | sinal=%d%% — tentando de novo\n", code, dt, p);
  return false;
}

void setup(){
  Serial.begin(115200); delay(500);
  Serial.println("\n=== TESTE RURAL: insiste ate enviar ao Supabase ===");
  if(conectarWiFi()){
    // tenta enviar, insistindo
    for(int i=1;i<=10;i++){
      Serial.printf("[ENVIO] tentativa %d/10 ... ", i);
      if(enviarSupabase()){ Serial.println(">>> SUCESSO! pacote no Supabase. <<<"); break; }
      if(WiFi.status()!=WL_CONNECTED){ Serial.println("(WiFi caiu, reconectando)"); conectarWiFi(); }
      delay(4000);
    }
  }
  Serial.println("=== FIM DO TESTE ===");
}

void loop(){ delay(1000); }
