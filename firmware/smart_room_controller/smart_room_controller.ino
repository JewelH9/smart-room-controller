#include "DHT.h"

#define DHT_PIN     4
#define PIR_PIN     27
#define LDR_PIN     34
#define FAN_RELAY   26
#define LIGHT_RELAY 25

#define RELAY_ON  HIGH
#define RELAY_OFF LOW

const float TEMP_ON  = 26.0;
const float TEMP_OFF = 25.0;
const int   DARK_THRESHOLD = 2000;
const unsigned long ROOM_TIMEOUT = 10000UL;
const unsigned long SENSOR_INTERVAL = 2000UL;

DHT dht(DHT_PIN, DHT22);

unsigned long lastMotionTime = 0;
unsigned long lastSensorRead = 0;
bool everSeenMotion = false;
bool occupied = false;
bool fanOn = false;
bool lightOn = false;
float temperature = 25.0;
int ldrValue = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(FAN_RELAY, OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);
  digitalWrite(FAN_RELAY, RELAY_OFF);
  digitalWrite(LIGHT_RELAY, RELAY_OFF);
  dht.begin();
  Serial.println("Smart Room Controller started");
}

void loop() {
  unsigned long now = millis();

  if (digitalRead(PIR_PIN) == HIGH) {
    lastMotionTime = now;
    everSeenMotion = true;
  }

  occupied = everSeenMotion && (now - lastMotionTime < ROOM_TIMEOUT);

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    float t = dht.readTemperature();
    if (!isnan(t)) temperature = t; 
    ldrValue = analogRead(LDR_PIN);

    Serial.printf("Temp: %.1f C | LDR: %d | Occupied: %s | Fan: %s | Light: %s\n",
                  temperature, ldrValue,
                  occupied ? "YES" : "NO",
                  fanOn ? "ON" : "OFF",
                  lightOn ? "ON" : "OFF");
  }

  if (!occupied) {
    fanOn = false;            
  } else if (temperature >= TEMP_ON) {
    fanOn = true;
  } else if (temperature <= TEMP_OFF) {
    fanOn = false;
  }                                     

  // 5. LIGHT logic
  if (!occupied) {
    lightOn = false;
  } else if (!lightOn && ldrValue > DARK_THRESHOLD) {
    lightOn = true;                    
  }

  digitalWrite(FAN_RELAY,   fanOn   ? RELAY_ON : RELAY_OFF);
  digitalWrite(LIGHT_RELAY, lightOn ? RELAY_ON : RELAY_OFF);
}
