#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"
#include "secrets.h"      // WIFI_SSID, WIFI_PASS, MQTT_BASE live in this file

// ================= PINS =================
#define DHT_PIN     4
#define PIR_PIN     27
#define LDR_PIN     34
#define FAN_RELAY   26
#define LIGHT_RELAY 25

// ============ HARDWARE BEHAVIOUR (from our own tests) ============
#define RELAY_ON       LOW    // relay clicks ON when the pin goes LOW
#define RELAY_OFF      HIGH
#define LDR_DARK_LEVEL HIGH   // our LDR module: 1 = dark, 0 = bright

// ================= SETTINGS =================
float tempOn  = 26.0;                       // fan ON at or above this
float tempOff = 25.0;                       // fan OFF at or below this
unsigned long roomTimeout = 10000UL;        // 10 s for testing. Later: 300000UL (5 min)
const unsigned long PIR_WARMUP      = 60000UL;  // ignore the PIR for the first 60 s
const unsigned long SENSOR_INTERVAL = 2000UL;   // DHT22 can only be read every ~2 s

// ================= NOISE FILTERS (the bug fixes) =================
const unsigned long RELAY_BLANK     = 3000UL;   // ignore the PIR for 3 s after a relay switches
const unsigned long PIR_STUCK_LIMIT = 30000UL;  // PIR HIGH this long without a break = treat as noise
const unsigned long LIGHT_ON_DELAY  = 3000UL;   // must be dark this long before "dark" counts
const unsigned long LIGHT_OFF_DELAY = 20000UL;  // must be bright this long before "bright" counts

// ================= MQTT =================
const char* MQTT_HOST    = "broker.hivemq.com";
const int   MQTT_PORT    = 1883;
const char* TOPIC_STATUS = MQTT_BASE "/status";
const char* TOPIC_ONLINE = MQTT_BASE "/online";
const char* TOPIC_FAN    = MQTT_BASE "/cmd/fan";
const char* TOPIC_LIGHT  = MQTT_BASE "/cmd/light";
const char* TOPIC_CFG    = MQTT_BASE "/cmd/config";

// ================= OBJECTS =================
DHT dht(DHT_PIN, DHT22);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// A manual mode can be AUTO (code decides), FORCE_ON or FORCE_OFF
enum Mode { AUTO, FORCE_ON, FORCE_OFF };

// ================= STATE (the system's memory) =================
unsigned long lastMotionTime = 0;
unsigned long lastSensorRead = 0;
unsigned long lastMqttTry    = 0;
unsigned long lastWifiTry    = 0;

bool everSeenMotion = false;
bool occupied  = false;       // someone is (recently) in the room
bool dark      = false;       // debounced darkness
bool autoFan   = false;       // what the automation wants
bool autoLight = false;
bool fanOn     = false;       // what the relays are really doing
bool lightOn   = false;
bool publishNow = false;

Mode fanMode   = AUTO;
Mode lightMode = AUTO;

float temperature = 25.0;
float humidity    = 50.0;

// State used by the noise filters
unsigned long lastRelayChange = 0;   // when a relay last switched
unsigned long pirHighSince    = 0;   // when the current PIR HIGH run started
bool pirWasHigh = false;
bool lastRawDark = false;            // last raw LDR reading
unsigned long lightChangeAt = 0;     // when the raw LDR reading last changed
bool lastFanOut = false, lastLightOut = false;

// ---------- Helper: Mode -> text ----------
const char* modeName(Mode m) {
  if (m == AUTO) return "AUTO";
  if (m == FORCE_ON) return "ON";
  return "OFF";
}

// ---------- Runs automatically when a message arrives ----------
void onMessage(char* topic, byte* payload, unsigned int len) {
  char text[64];
  if (len >= sizeof(text)) return;         // ignore messages that are too long
  memcpy(text, payload, len);
  text[len] = 0;                           // end of text marker
  String t = String(topic);

  // Settings message looks like: "27,26,300" = tempOn, tempOff, timeout in seconds
  if (t == TOPIC_CFG) {
    float a, b;
    unsigned long c;
    if (sscanf(text, "%f,%f,%lu", &a, &b, &c) == 3 &&
        a > b && a <= 40 && b >= 10 && c >= 5 && c <= 3600) {   // never trust incoming data
      tempOn = a;
      tempOff = b;
      roomTimeout = c * 1000UL;
      Serial.printf("New settings: on=%.1f off=%.1f timeout=%lus\n", a, b, c);
      publishNow = true;
    }
    return;
  }

  // Otherwise it is a fan or light command: ON, OFF or AUTO
  String msg = String(text);
  msg.trim();
  msg.toUpperCase();

  Mode m;
  if (msg == "ON") m = FORCE_ON;
  else if (msg == "OFF") m = FORCE_OFF;
  else if (msg == "AUTO") m = AUTO;
  else return;                             // unknown word: ignore

  if (t == TOPIC_FAN) fanMode = m;
  if (t == TOPIC_LIGHT) lightMode = m;
  Serial.printf("Command %s -> %s\n", topic, msg.c_str());
  publishNow = true;                       // tell the dashboard straight away
}

// ---------- WiFi ----------
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(500);                            // fine here: we only wait during startup
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" FAILED. Automation still works without WiFi.");
  }
}

// ---------- MQTT (tries every 5 s, never blocks the automation) ----------
void ensureMqtt(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {            // WiFi lost: retry it every 10 s
    if (now - lastWifiTry > 10000UL) {
      lastWifiTry = now;
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (mqtt.connected()) return;
  if (now - lastMqttTry < 5000UL) return;
  lastMqttTry = now;

  String clientId = "esp32-room-" + String((uint32_t)esp_random(), HEX);  // must be unique
  Serial.print("Connecting to MQTT... ");

  // Last Will: if we disappear, the BROKER announces "offline" for us
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

// ---------- Read sensors every 2 s. Returns true on each new reading ----------
bool readSensors(unsigned long now) {
  if (now - lastSensorRead < SENSOR_INTERVAL) return false;
  lastSensorRead = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;          // ignore failed reads
  if (!isnan(h)) humidity = h;
  return true;
}

// ---------- Is someone in the room? (with noise filters) ----------
void updateOccupancy(unsigned long now) {
  if (now < PIR_WARMUP) {                  // PIR gives false alarms while warming up
    occupied = false;
    return;
  }

  bool pirHigh = (digitalRead(PIR_PIN) == HIGH);

  // Remember when the current HIGH run started
  if (pirHigh && !pirWasHigh) pirHighSince = now;
  pirWasHigh = pirHigh;

  // Filter 1: a relay just switched, so the reading may be electrical noise
  bool blanked = (now - lastRelayChange < RELAY_BLANK);

  // Filter 2: a person pauses now and then, a fan never does.
  // If the PIR has been HIGH non-stop for too long, stop counting it.
  bool stuck = pirHigh && (now - pirHighSince > PIR_STUCK_LIMIT);

  if (pirHigh && !blanked && !stuck) {     // only real-looking motion restarts the timer
    lastMotionTime = now;
    everSeenMotion = true;
  }
  occupied = everSeenMotion && (now - lastMotionTime < roomTimeout);
}

// ---------- The brain: decide what the fan and light should do ----------
void runAutomation(unsigned long now) {
  // Read the LDR, but only believe a change after it has lasted a while ("debounce")
  bool rawDark = (digitalRead(LDR_PIN) == LDR_DARK_LEVEL);
  if (rawDark != lastRawDark) {            // the reading just changed: restart the clock
    lastRawDark = rawDark;
    lightChangeAt = now;
  }
  unsigned long stableFor = now - lightChangeAt;
  if (rawDark && stableFor >= LIGHT_ON_DELAY) dark = true;
  if (!rawDark && stableFor >= LIGHT_OFF_DELAY) dark = false;
  // in between, 'dark' keeps its old value

  // FAN: hysteresis (ON at tempOn, OFF at tempOff, hold in between)
  if (!occupied) autoFan = false;
  else if (temperature >= tempOn) autoFan = true;
  else if (temperature <= tempOff) autoFan = false;

  // LIGHT: ON only when someone is in the room AND it is dark.
  // Daylight turns it off (the old "stay on until empty" latch is gone).
  autoLight = occupied && dark;

  // A manual command beats AUTO
  if (fanMode == AUTO) fanOn = autoFan;
  else fanOn = (fanMode == FORCE_ON);

  if (lightMode == AUTO) lightOn = autoLight;
  else lightOn = (lightMode == FORCE_ON);
}

// ---------- Switch the relays (and note when one changes) ----------
void driveRelays(unsigned long now) {
  if (fanOn != lastFanOut || lightOn != lastLightOut) {
    lastRelayChange = now;                 // the PIR filter uses this
    lastFanOut = fanOn;
    lastLightOut = lightOn;
  }
  digitalWrite(FAN_RELAY,   fanOn   ? RELAY_ON : RELAY_OFF);
  digitalWrite(LIGHT_RELAY, lightOn ? RELAY_ON : RELAY_OFF);
}

// ---------- Send the status to the dashboard as JSON text ----------
void publishStatus(unsigned long now) {
  unsigned long left = 0;
  if (occupied) left = (roomTimeout - (now - lastMotionTime)) / 1000UL;

  char json[360];
  snprintf(json, sizeof(json),
    "{\"temp\":%.1f,\"hum\":%.0f,\"ldr\":%d,\"dark\":%s,\"occupied\":%s,\"left\":%lu,"
    "\"fan\":%s,\"light\":%s,\"fanMode\":\"%s\",\"lightMode\":\"%s\","
    "\"tOn\":%.1f,\"tOff\":%.1f,\"timeout\":%lu}",
    temperature, humidity,
    dark ? 4095 : 0,                       // digital LDR: fake a full or empty light bar
    dark ? "true" : "false",
    occupied ? "true" : "false", left,
    fanOn ? "true" : "false", lightOn ? "true" : "false",
    modeName(fanMode), modeName(lightMode),
    tempOn, tempOff, roomTimeout / 1000UL);

  mqtt.publish(TOPIC_STATUS, json, true);  // true = retained (new viewers get it at once)
  Serial.println(json);
}

// ================= SETUP: runs once =================
void setup() {
  Serial.begin(115200);

  // Relays first: set the level BEFORE pinMode so they never click at boot
  digitalWrite(FAN_RELAY, RELAY_OFF);   pinMode(FAN_RELAY, OUTPUT);
  digitalWrite(LIGHT_RELAY, RELAY_OFF); pinMode(LIGHT_RELAY, OUTPUT);

  pinMode(PIR_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  dht.begin();

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);                 // default is too small for our JSON
  mqtt.setCallback(onMessage);
  Serial.println("PIR warming up for 60 s. Keep out of its view.");
}

// ================= LOOP: repeats forever =================
void loop() {
  unsigned long now = millis();

  ensureMqtt(now);
  mqtt.loop();                             // keeps MQTT alive and delivers messages

  updateOccupancy(now);
  bool newReading = readSensors(now);
  runAutomation(now);
  driveRelays(now);

  if ((newReading || publishNow) && mqtt.connected()) {
    publishNow = false;
    publishStatus(now);
  }
}