#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"

// ---------- WiFi (Wokwi's virtual network) ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ---------- MQTT ----------
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
#define BASE "jewelh9/room1"                 // keep same as dashboard
const char* TOPIC_STATUS = BASE "/status";
const char* TOPIC_ONLINE = BASE "/online";
const char* TOPIC_FAN    = BASE "/cmd/fan";
const char* TOPIC_LIGHT  = BASE "/cmd/light";
const char* TOPIC_CFG    = BASE "/cmd/config";

// ---------- Pins ----------
#define DHT_PIN     4
#define PIR_PIN     27
#define LDR_PIN     34
#define FAN_RELAY   26
#define LIGHT_RELAY 25

// Wokwi relay is active HIGH. Real modules are usually active LOW: swap these.
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// ---------- Settings (now changeable from the dashboard) ----------
float tempOn  = 26.0;
float tempOff = 25.0;
unsigned long roomTimeout = 10000UL;        // ms. Dashboard can change it
const int DARK_THRESHOLD = 2000;
const unsigned long SENSOR_INTERVAL = 2000UL;

enum Mode { AUTO, FORCE_ON, FORCE_OFF };

DHT dht(DHT_PIN, DHT22);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- State ----------
unsigned long lastMotionTime = 0, lastSensorRead = 0, lastReconnect = 0;
bool everSeenMotion = false, occupied = false;
bool autoFan = false, autoLight = false;
bool fanOn = false, lightOn = false;
Mode fanMode = AUTO, lightMode = AUTO;
float temperature = 25.0, humidity = 50.0;
int ldrValue = 0;
bool publishNow = false;

const char* modeName(Mode m) {
  return m == AUTO ? "AUTO" : (m == FORCE_ON ? "ON" : "OFF");
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  char buf[64];
  if (len >= sizeof(buf)) return;
  memcpy(buf, payload, len);
  buf[len] = 0;
  String t(topic);

  // Config message format: "tempOn,tempOff,timeoutSeconds"  e.g. "27,26,300"
  if (t == TOPIC_CFG) {
    float a, b; unsigned long c;
    if (sscanf(buf, "%f,%f,%lu", &a, &b, &c) == 3 &&
        a > b && a <= 40 && b >= 10 && c >= 5 && c <= 3600) {
      tempOn = a; tempOff = b; roomTimeout = c * 1000UL;
      Serial.printf("Config: on=%.1f off=%.1f timeout=%lus\n", a, b, c);
      publishNow = true;
    }
    return;                                  // invalid values are ignored
  }

  String msg(buf);
  msg.trim();
  msg.toUpperCase();
  Mode m;
  if (msg == "ON") m = FORCE_ON;
  else if (msg == "OFF") m = FORCE_OFF;
  else if (msg == "AUTO") m = AUTO;
  else return;

  if (t == TOPIC_FAN) fanMode = m;
  else if (t == TOPIC_LIGHT) lightMode = m;
  Serial.printf("Command on %s: %s\n", topic, msg.c_str());
  publishNow = true;
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected");
}

void ensureMqtt(unsigned long now) {
  if (mqtt.connected()) return;
  if (now - lastReconnect < 5000UL) return;
  lastReconnect = now;

  String clientId = "esp32-room1-" + String((uint32_t)esp_random(), HEX);
  Serial.print("Connecting to MQTT... ");
  // Last Will: if we vanish, the BROKER publishes "offline" (retained) for us
  if (mqtt.connect(clientId.c_str(), NULL, NULL, TOPIC_ONLINE, 0, true, "offline")) {
    Serial.println("ok");
    mqtt.publish(TOPIC_ONLINE, "online", true);
    mqtt.subscribe(TOPIC_FAN);
    mqtt.subscribe(TOPIC_LIGHT);
    mqtt.subscribe(TOPIC_CFG);
    publishNow = true;
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

void publishStatus(unsigned long now) {
  unsigned long left = occupied ? (roomTimeout - (now - lastMotionTime)) / 1000UL : 0;
  char json[360];
  snprintf(json, sizeof(json),
    "{\"temp\":%.1f,\"hum\":%.0f,\"ldr\":%d,\"dark\":%s,\"occupied\":%s,\"left\":%lu,"
    "\"fan\":%s,\"light\":%s,\"fanMode\":\"%s\",\"lightMode\":\"%s\","
    "\"tOn\":%.1f,\"tOff\":%.1f,\"timeout\":%lu}",
    temperature, humidity, ldrValue,
    ldrValue > DARK_THRESHOLD ? "true" : "false",
    occupied ? "true" : "false", left,
    fanOn ? "true" : "false", lightOn ? "true" : "false",
    modeName(fanMode), modeName(lightMode),
    tempOn, tempOff, roomTimeout / 1000UL);
  mqtt.publish(TOPIC_STATUS, json, true);    // true = retained
  Serial.println(json);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(FAN_RELAY, OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);
  digitalWrite(FAN_RELAY, RELAY_OFF);
  digitalWrite(LIGHT_RELAY, RELAY_OFF);
  dht.begin();

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);                   // default 256 is too small for our JSON
  mqtt.setCallback(onMessage);
}

void loop() {
  unsigned long now = millis();
  ensureMqtt(now);
  mqtt.loop();

  if (digitalRead(PIR_PIN) == HIGH) {
    lastMotionTime = now;
    everSeenMotion = true;
  }
  occupied = everSeenMotion && (now - lastMotionTime < roomTimeout);

  bool sensorTick = false;
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    sensorTick = true;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;
    if (!isnan(h)) humidity = h;
    ldrValue = analogRead(LDR_PIN);
  }

  if (!occupied) autoFan = false;
  else if (temperature >= tempOn) autoFan = true;
  else if (temperature <= tempOff) autoFan = false;

  if (!occupied) autoLight = false;
  else if (!autoLight && ldrValue > DARK_THRESHOLD) autoLight = true;

  fanOn   = (fanMode   == AUTO) ? autoFan   : (fanMode   == FORCE_ON);
  lightOn = (lightMode == AUTO) ? autoLight : (lightMode == FORCE_ON);

  digitalWrite(FAN_RELAY,   fanOn   ? RELAY_ON : RELAY_OFF);
  digitalWrite(LIGHT_RELAY, lightOn ? RELAY_ON : RELAY_OFF);

  if ((sensorTick || publishNow) && mqtt.connected()) {
    publishNow = false;
    publishStatus(now);
  }
}