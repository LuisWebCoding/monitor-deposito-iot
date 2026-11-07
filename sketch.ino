/* Monitor do Deposito - Caminho B: MQTT no HiveMQ
   Matricula: 0020016349
   DHT22: GPIO 15; LED alarme: GPIO 2; buzzer: GPIO 4;
   LED de comando: GPIO 16. Limite: 29 C, histerese: 1 C.
   Leitura: 2 s. Telemetria: 5 s. Sem credenciais reais.
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include "DHTesp.h"
#include <math.h>

const char* MATRICULA = "0020016349";
const char* BROKER = "broker.hivemq.com";
const int PORTA = 1883;
const float LIMITE = 29.0;
const int PINO_DHT = 15, PINO_LED = 2, PINO_BUZZER = 4, PINO_LED_CMD = 16;

DHTesp dht;
WiFiClient wifi;
PubSubClient mqtt(wifi);
char clientId[64], tCheckin[80], tTelemetria[80], tCmd[80];
char tAck[80], tAlerta[80], tStatus[80];
char alertaPendente[128];
bool alarme = false, leituraValida = false, enviarAlerta = false;
float temperatura = 0, umidade = 0;
unsigned long ultimaLeitura = 0, ultimoEnvio = 0;
unsigned long tentativaMqtt = 0, tentativaWifi = 0;

void montarTopicos() {
  // Sufixo aleatorio reduz conflitos entre simulacoes simultaneas.
  snprintf(clientId, sizeof(clientId), "esp32-%s-%08lx", MATRICULA,
           (unsigned long)esp_random());
  snprintf(tCheckin, sizeof(tCheckin), "senac/iot/%s/checkin", MATRICULA);
  snprintf(tTelemetria, sizeof(tTelemetria), "senac/iot/%s/sala1/telemetria", MATRICULA);
  snprintf(tCmd, sizeof(tCmd), "senac/iot/%s/sala1/cmd/led", MATRICULA);
  snprintf(tAck, sizeof(tAck), "senac/iot/%s/sala1/ack", MATRICULA);
  snprintf(tAlerta, sizeof(tAlerta), "senac/iot/%s/sala1/alerta", MATRICULA);
  snprintf(tStatus, sizeof(tStatus), "senac/iot/%s/status", MATRICULA);
}

void aoReceber(char* topico, byte* payload, unsigned int tamanho) {
  if (strcmp(topico, tCmd) != 0 || tamanho >= 16) return;
  char msg[16];
  memcpy(msg, payload, tamanho);
  msg[tamanho] = '\0';
  if (strcmp(msg, "ON") == 0) {
    digitalWrite(PINO_LED_CMD, HIGH);
    mqtt.publish(tAck, "LED=ON");
  } else if (strcmp(msg, "OFF") == 0) {
    digitalWrite(PINO_LED_CMD, LOW);
    mqtt.publish(tAck, "LED=OFF");
  } else {
    mqtt.publish(tAck, "ERRO: use ON ou OFF");
  }
  Serial.printf("Comando: %s\n", msg);
}

void conectarMqtt() {
  if (!mqtt.connect(clientId, NULL, NULL, tStatus, 0, true, "offline")) {
    Serial.printf("Falha MQTT: %d\n", mqtt.state());
    return;
  }
  mqtt.publish(tStatus, "online", true);
  mqtt.subscribe(tCmd);
  char checkin[192];
  snprintf(checkin, sizeof(checkin),
    "{\"mat\":\"%s\",\"limite\":29,\"clientId\":\"%s\",\"base\":3}",
    MATRICULA, clientId);
  mqtt.publish(tCheckin, checkin, true);
  Serial.println("MQTT conectado; check-in enviado.");
}

void lerSensor() {
  TempAndHumidity leitura = dht.getTempAndHumidity();
  if (dht.getStatus() != DHTesp::ERROR_NONE ||
      !isfinite(leitura.temperature) || !isfinite(leitura.humidity) ||
      leitura.temperature < -40 || leitura.temperature > 80 ||
      leitura.humidity < 0 || leitura.humidity > 100) {
    leituraValida = false;
    Serial.println("Leitura invalida ignorada; alarme mantido.");
    return;
  }
  temperatura = leitura.temperature;
  umidade = leitura.humidity;
  leituraValida = true;
  bool novoAlarme = alarme;
  if (!alarme && temperatura > LIMITE) novoAlarme = true;
  if (alarme && temperatura < LIMITE - 1.0) novoAlarme = false;

  if (!alarme && novoAlarme) {
    snprintf(alertaPendente, sizeof(alertaPendente),
      "{\"mat\":\"%s\",\"evento\":\"ALERTA\",\"t\":%.1f,\"limite\":29}",
      MATRICULA, temperatura);
    enviarAlerta = true;
    Serial.printf("ALARME ATIVADO: %s\n", alertaPendente);
  }
  alarme = novoAlarme;
  digitalWrite(PINO_LED, alarme ? HIGH : LOW);
  if (alarme) tone(PINO_BUZZER, 1000, 200);
  else noTone(PINO_BUZZER);
  Serial.printf("Matricula=%s | T=%.1f C | H=%.1f %% | Alarme=%s\n",
    MATRICULA, temperatura, umidade, alarme ? "LIGADO" : "DESLIGADO");
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_LED_CMD, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  digitalWrite(PINO_LED, LOW);
  digitalWrite(PINO_LED_CMD, LOW);
  dht.setup(PINO_DHT, DHTesp::DHT22);
  montarTopicos();
  Serial.printf("MONITOR DO DEPOSITO | Matricula=%s | Limite=29 C\n", MATRICULA);
  Serial.printf("Assine no cliente web: senac/iot/%s/#\n", MATRICULA);
  mqtt.setServer(BROKER, PORTA);
  mqtt.setBufferSize(512);
  mqtt.setCallback(aoReceber);
  mqtt.setSocketTimeout(1);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin("Wokwi-GUEST", "", 6);
}

void loop() {
  unsigned long agora = millis();
  // Mede mesmo enquanto a rede esta indisponivel.
  if (agora - ultimaLeitura >= 2000) {
    ultimaLeitura = agora;
    lerSensor();
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (agora - tentativaWifi >= 10000) {
      tentativaWifi = agora;
      WiFi.reconnect();
    }
    return;
  }
  if (!mqtt.connected() && agora - tentativaMqtt >= 5000) {
    tentativaMqtt = agora;
    conectarMqtt();
  }
  mqtt.loop();
  // Uma publicacao por ativacao; guarda o ultimo evento pendente em RAM.
  // QoS 0 nao garante entrega; publish=true nao e confirmacao do gestor.
  if (mqtt.connected() && enviarAlerta) {
    if (mqtt.publish(tAlerta, alertaPendente)) enviarAlerta = false;
  }
  if (mqtt.connected() && leituraValida && agora - ultimoEnvio >= 5000) {
    ultimoEnvio = agora;
    char json[160];
    snprintf(json, sizeof(json),
      "{\"mat\":\"%s\",\"t\":%.1f,\"h\":%.1f,\"alarme\":%d}",
      MATRICULA, temperatura, umidade, alarme ? 1 : 0);
    bool ok = mqtt.publish(tTelemetria, json);
    Serial.printf("Telemetria %s: %s\n", ok ? "enviada" : "falhou", json);
  }
}
