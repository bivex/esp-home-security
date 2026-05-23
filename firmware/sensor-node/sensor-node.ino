/*
 * ESP8266 Home Security — Sensor Node
 *
 * Hardware: ESP-12F (NodeMCU / Wemos D1 Mini)
 * Sensors: HC-SR501 PIR (motion), Reed switch (door/window)
 * Power:  18650 battery + TP4056 charger OR USB 5V
 *
 * Pin mapping:
 *   PIR output:  GPIO 14 (D5)
 *   Reed switch: GPIO 12 (D6) — INPUT_PULLUP, LOW = closed (magnet near)
 *   Battery ADC: GPIO 17 (A0) via voltage divider
 *   Status LED:  GPIO 2  (built-in, active LOW)
 *
 * MQTT topics published:
 *   home/security/sensor/{NODE_ID}/motion — "detected" / "clear"
 *   home/security/sensor/{NODE_ID}/door   — "open" / "closed"
 *   home/security/sensor/{NODE_ID}/window — "open" / "closed"
 *   home/security/sensor/{NODE_ID}/battery — "3.72"
 *   home/security/sensor/{NODE_ID}/status — "online" / "offline" (LWT)
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include "config.h"

#define PIR_PIN      14  // D5
#define REED_PIN     12  // D6
#define LED_PIN      2   // built-in
#define DEBOUNCE_MS  50

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------- state ----------
bool pirState = false;
bool pirLastPublished = false;
bool reedState = false;
bool reedLastPublished = false;
unsigned long lastDebouncePIR = 0;
unsigned long lastDebounceReed = 0;
unsigned long lastBatteryReport = 0;
const unsigned long BATTERY_INTERVAL = 3600000; // 1 hour

String motionTopic;
String reedTopic;
String batteryTopic;

// ---------- helpers ----------
void blink(int count, int ms) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, LOW); delay(ms);
    digitalWrite(LED_PIN, HIGH); delay(ms);
  }
}

float readBatteryVoltage() {
  // Voltage divider: R1=100k, R2=100k → factor 2.0
  // ESP8266 ADC: 0-1023 = 0-1V on A0
  // With divider: Vbat = analogRead * (1.0/1024) * 2.0 * calibration
  int raw = analogRead(A0);
  return (raw / 1024.0) * 2.0 * 4.2 / 1.0; // adjust calibration factor
}

void publish(const char* topic, const char* payload, bool retained = false) {
  if (mqtt.connected()) {
    mqtt.publish(topic, payload, retained);
  }
}

// ---------- WiFi ----------
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) blink(3, 100);
}

// ---------- MQTT ----------
void setupMQTT() {
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setSocketTimeout(10);
}

void reconnectMQTT() {
  String lwtTopic = "home/security/sensor/" NODE_ID "/status";
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    if (mqtt.connect(NODE_ID, MQTT_USER, MQTT_PASS,
                     lwtTopic.c_str(), 1, true, "offline")) {
      mqtt.publish(lwtTopic.c_str(), "online", true);
    } else {
      delay(2000);
    }
    attempts++;
  }
}

// ---------- sensors ----------
void readSensors() {
  // PIR motion
  bool pirRaw = digitalRead(PIR_PIN);
  if (pirRaw != pirState) {
    lastDebouncePIR = millis();
  }
  if (millis() - lastDebouncePIR > DEBOUNCE_MS) {
    if (pirRaw != pirState) {
      pirState = pirRaw;
      publish(motionTopic.c_str(), pirState ? "detected" : "clear");
    }
  }

  // Reed switch (door/window)
  bool reedRaw = digitalRead(REED_PIN) == LOW; // LOW = magnet near = closed
  if (reedRaw != reedState) {
    lastDebounceReed = millis();
  }
  if (millis() - lastDebounceReed > DEBOUNCE_MS) {
    if (reedRaw != reedState) {
      reedState = reedRaw;
      // Reed reports "open" when magnet removed (door/window opened)
      publish(reedTopic.c_str(), reedState ? "closed" : "open");
    }
  }
}

void reportBattery() {
  if (millis() - lastBatteryReport > BATTERY_INTERVAL) {
    float v = readBatteryVoltage();
    char buf[8];
    dtostrf(v, 1, 2, buf);
    publish(batteryTopic.c_str(), buf);
    lastBatteryReport = millis();
  }
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED off (active LOW)

  motionTopic  = "home/security/sensor/" NODE_ID "/motion";
  reedTopic    = "home/security/sensor/" NODE_ID "/" REED_TYPE;
  batteryTopic = "home/security/sensor/" NODE_ID "/battery";

  setupWiFi();
  setupMQTT();
  reconnectMQTT();

  // Publish initial states
  pirState = digitalRead(PIR_PIN);
  reedState = digitalRead(REED_PIN) == LOW;
  publish(motionTopic.c_str(), pirState ? "detected" : "clear", true);
  publish(reedTopic.c_str(), reedState ? "closed" : "open", true);
}

// ---------- loop ----------
void loop() {
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();
  readSensors();
  reportBattery();
  delay(10);
}
