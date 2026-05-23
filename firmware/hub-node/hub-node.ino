/*
 * ESP32 Home Security — Central Hub Node
 *
 * Hardware: ESP32 DevKit V1
 * Connected: 4x4 Keypad, Buzzer/Siren via MOSFET, Status LED, OLED display
 * Protocol: MQTT over WiFi
 *
 * Pin mapping:
 *   Keypad ROW: GPIO 13, 12, 14, 27
 *   Keypad COL: GPIO 26, 25, 33, 32
 *   Siren:      GPIO 15 (via IRLZ44N MOSFET)
 *   Status LED: GPIO 2 (built-in)
 *   OLED SDA:   GPIO 21
 *   OLED SCL:   GPIO 22
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Keypad.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============ CONFIGURATION ============
// Copy config.h.example to config.h and fill in your values
#include "config.h"

// ============ HARDWARE ============
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SIREN_PIN 15
#define LED_PIN 2

Adafruit_SSD1306 display(SCREEN_WIDTH, 64, &Wire, -1);

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS]  = {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ============ STATE ============
enum SystemMode { MODE_DISARMED, MODE_ARMED_AWAY, MODE_ARMED_HOME };
SystemMode sysMode = MODE_DISARMED;

String pinCode = "";
const String MASTER_PIN = "1234";   // change via MQTT
const int MAX_PIN_LEN = 6;
unsigned long lastKeyTime = 0;
const unsigned long KEY_TIMEOUT = 10000; // 10s to enter PIN

bool alarmActive = false;
unsigned long alarmStartTime = 0;
const unsigned long ALARM_DURATION = 180000; // 3 min auto-off

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ============ MQTT TOPICS ============
// home/security/hub/status       — online/offline (LWT)
// home/security/hub/mode         — disarmed/armed_away/armed_home
// home/security/hub/alarm        — on/off
// home/security/sensor/+/motion  — from sensor nodes
// home/security/sensor/+/door    — from sensor nodes
// home/security/sensor/+/window  — from sensor nodes
// home/security/camera/+/capture — trigger camera snapshot
// home/security/hub/config       — receive config updates

// ============ OLED DISPLAY ============
void displayStatus() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Mode indicator
  switch (sysMode) {
    case MODE_DISARMED:
      display.println("MODE: DISARMED");
      break;
    case MODE_ARMED_AWAY:
      display.println("MODE: ARMED AWAY");
      break;
    case MODE_ARMED_HOME:
      display.println("MODE: ARMED HOME");
      break;
  }

  if (alarmActive) {
    display.println("** ALARM ACTIVE **");
  }

  if (pinCode.length() > 0) {
    display.print("PIN: ");
    for (unsigned int i = 0; i < pinCode.length(); i++) display.print("*");
    display.println();
  }

  display.print("WiFi: ");
  display.println(WiFi.isConnected() ? "OK" : "FAIL");
  display.print("MQTT: ");
  display.println(mqtt.connected() ? "OK" : "FAIL");
  display.display();
}

// ============ ALARM ============
void activateAlarm() {
  if (alarmActive) return;
  alarmActive = true;
  alarmStartTime = millis();
  digitalWrite(SIREN_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);
  mqtt.publish("home/security/hub/alarm", "on", true);
  displayStatus();
}

void deactivateAlarm() {
  alarmActive = false;
  digitalWrite(SIREN_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  mqtt.publish("home/security/hub/alarm", "off", true);
  displayStatus();
}

void setMode(SystemMode mode) {
  sysMode = mode;
  deactivateAlarm();
  const char* modeStr;
  switch (mode) {
    case MODE_DISARMED:  modeStr = "disarmed";   break;
    case MODE_ARMED_AWAY: modeStr = "armed_away"; break;
    case MODE_ARMED_HOME: modeStr = "armed_home"; break;
  }
  mqtt.publish("home/security/hub/mode", modeStr, true);
  displayStatus();
}

// ============ KEYPAD ============
void processKey(char key) {
  lastKeyTime = millis();

  if (key == '#') {
    // Submit PIN
    if (pinCode == MASTER_PIN) {
      if (sysMode == MODE_DISARMED) {
        setMode(MODE_ARMED_AWAY);
      } else {
        setMode(MODE_DISARMED);
      }
    } else if (pinCode == "00") {
      setMode(MODE_ARMED_HOME);
    }
    pinCode = "";
  } else if (key == '*') {
    pinCode = "";
  } else if (pinCode.length() < MAX_PIN_LEN) {
    pinCode += key;
  }
  displayStatus();
}

// ============ SENSOR EVENT HANDLER ============
void handleSensorEvent(const char* topic, const char* payload) {
  if (sysMode == MODE_DISARMED) return;
  if (strcmp(payload, "clear") == 0) return;

  // In armed_home mode, ignore interior motion sensors
  // (only react to door/window/perimeter sensors)
  String topicStr = topic;
  bool isPerimeter = topicStr.indexOf("/door") >= 0 || topicStr.indexOf("/window") >= 0;

  if (sysMode == MODE_ARMED_HOME && !isPerimeter) return;

  // Trigger alarm
  activateAlarm();

  // Request camera snapshot
  mqtt.publish("home/security/camera/cam1/capture", "1");

  // Send Telegram-style alert via MQTT (bridge on server handles delivery)
  String alert = "ALERT: ";
  alert += topic;
  alert += " = ";
  alert += payload;
  mqtt.publish("home/security/hub/alert", alert.c_str());
}

// ============ MQTT CALLBACK ============
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[256];
  int len = min((int)length, 255);
  memcpy(msg, payload, len);
  msg[len] = '\0';

  String t = topic;

  if (t.startsWith("home/security/sensor/")) {
    handleSensorEvent(topic, msg);
  } else if (t == "home/security/hub/config") {
    // Remote config updates
    if (String(msg) == "disarm") setMode(MODE_DISARMED);
    else if (String(msg) == "arm_away") setMode(MODE_ARMED_AWAY);
    else if (String(msg) == "arm_home") setMode(MODE_ARMED_HOME);
    else if (String(msg) == "alarm_on") activateAlarm();
    else if (String(msg) == "alarm_off") deactivateAlarm();
  }
}

// ============ WIFI ============
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    attempts++;
  }
}

// ============ MQTT ============
void setupMQTT() {
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(10);
}

void reconnectMQTT() {
  while (!mqtt.connected()) {
    if (mqtt.connect("esp32-hub", MQTT_USER, MQTT_PASS,
                      "home/security/hub/status", 1, true, "offline")) {
      mqtt.publish("home/security/hub/status", "online", true);
      mqtt.subscribe("home/security/sensor/#");
      mqtt.subscribe("home/security/hub/config");
    } else {
      delay(3000);
    }
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  pinMode(SIREN_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(SIREN_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  displayStatus();

  setupWiFi();
  setupMQTT();
}

// ============ LOOP ============
void loop() {
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  // Keypad input
  char key = keypad.getKey();
  if (key) processKey(key);

  // PIN entry timeout
  if (pinCode.length() > 0 && millis() - lastKeyTime > KEY_TIMEOUT) {
    pinCode = "";
    displayStatus();
  }

  // Auto-off alarm after duration
  if (alarmActive && millis() - alarmStartTime > ALARM_DURATION) {
    deactivateAlarm();
  }

  delay(10);
}
