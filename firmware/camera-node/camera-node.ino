/*
 * ESP32-CAM Home Security — Camera Node
 *
 * Hardware: AI-Thinker ESP32-CAM (OV2640)
 * Functions:
 *   1. MJPEG HTTP stream on port 80
 *   2. Capture photo on MQTT trigger, publish via MQTT base64 or HTTP POST
 *
 * Pin mapping (AI-Thinker board):
 *   OV2640 D0-D7: GPIO 5, 18, 12, 15, 16, 17, 19, 21
 *   OV2640 PCLK:  GPIO 22
 *   OV2640 HREF:  GPIO 26
 *   OV2640 VSYNC: GPIO 25
 *   OV2640 SDA/SCL: GPIO 26, 27 (shared with HREF via I2C)
 *   Flash LED:    GPIO 4
 *   SD Card:      GPIO 2 (CS), 14 (CLK), 15 (MOSI), 4 (MISO) — conflicts!
 *
 * IMPORTANT: Flash LED (GPIO 4) and SD card MISO share the same pin.
 *            If using SD card, don't use flash. Or use SD in 1-bit mode.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_camera.h>
#include "camera_pins.h"
#include "config.h"

// ============ CAMERA MODEL ============
#define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_AI_THINKER)
  #define PWDN_GPIO_NUM    32
  #define RESET_GPIO_NUM   -1
  #define XCLK_GPIO_NUM     0
  #define SIOD_GPIO_NUM    26
  #define SIOC_GPIO_NUM    27
  #define Y9_GPIO_NUM      35
  #define Y8_GPIO_NUM      34
  #define Y7_GPIO_NUM      39
  #define Y6_GPIO_NUM      36
  #define Y5_GPIO_NUM      21
  #define Y4_GPIO_NUM      19
  #define Y3_GPIO_NUM      18
  #define Y2_GPIO_NUM       5
  #define VSYNC_GPIO_NUM   25
  #define HREF_GPIO_NUM    23
  #define PCLK_GPIO_NUM    22
#endif

#define FLASH_PIN 4
#define LED_PIN 33

WiFiClient espClient;
PubSubClient mqtt(espClient);
WiFiServer httpServer(80);

String captureTopic = "home/security/camera/" NODE_ID "/capture";
String photoTopic   = "home/security/camera/" NODE_ID "/photo";
String statusTopic  = "home/security/camera/" NODE_ID "/status";

// ============ CAMERA INIT ============
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // QVGA for fast capture, can switch to VGA for quality
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 12;  // 0-63, lower = better
  config.fb_count     = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_brightness(s, 0);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  return true;
}

// ============ CAPTURE PHOTO ============
void captureAndSend() {
  digitalWrite(FLASH_PIN, HIGH);
  delay(100);

  camera_fb_t *fb = esp_camera_fb();
  digitalWrite(FLASH_PIN, LOW);

  if (!fb) {
    Serial.println("Capture failed");
    mqtt.publish(photoTopic.c_str(), "ERROR: capture failed");
    return;
  }

  Serial.printf("Captured %d bytes, %dx%d\n", fb->len, fb->width, fb->height);

  // Send via HTTP POST to server (more reliable than MQTT for large payloads)
  if (strlen(SERVER_URL) > 0) {
    HTTPClient http;
    http.setTimeout(10000);
    String url = String(SERVER_URL) + "/api/photo/" NODE_ID;
    http.begin(url);
    http.addHeader("Content-Type", "image/jpeg");
    int code = http.POST(fb->buf, fb->len);
    Serial.printf("HTTP POST: %d\n", code);
    http.end();
  }

  // Also publish small notification via MQTT
  mqtt.publish(photoTopic.c_str(), "captured");

  esp_camera_fb_return(fb);
}

// ============ MJPEG STREAM ============
void handleStream(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Connection: close");
  client.println();

  String captureReq = "home/security/camera/" NODE_ID "/stream";
  bool streaming = true;

  while (streaming && client.connected()) {
    camera_fb_t *fb = esp_camera_fb();
    if (!fb) {
      Serial.println("Stream frame failed");
      break;
    }

    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.printf("Content-Length: %d\n\n", fb->len);
    client.write(fb->buf, fb->len);
    client.println();

    esp_camera_fb_return(fb);
    delay(50); // ~20 FPS max
  }
}

void handleHttpRequest() {
  WiFiClient client = httpServer.available();
  if (!client) return;

  String req = client.readStringUntil('\n');
  req.trim();

  if (req.startsWith("GET /stream")) {
    handleStream(client);
  } else if (req.startsWith("GET /capture")) {
    // Single snapshot
    digitalWrite(FLASH_PIN, HIGH);
    delay(100);
    camera_fb_t *fb = esp_camera_fb();
    digitalWrite(FLASH_PIN, LOW);

    if (fb) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: image/jpeg");
      client.printf("Content-Length: %d\n\n", fb->len);
      client.write(fb->buf, fb->len);
      esp_camera_fb_return(fb);
    } else {
      client.println("HTTP/1.1 500 Error");
    }
  } else {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println();
    client.println("ESP32-CAM " NODE_ID);
    client.println("  /stream  - MJPEG stream");
    client.println("  /capture - single JPEG");
  }
  client.stop();
}

// ============ MQTT ============
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = topic;
  if (t == captureTopic) {
    captureAndSend();
  }
}

void reconnectMQTT() {
  int attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    if (mqtt.connect(NODE_ID, MQTT_USER, MQTT_PASS,
                     statusTopic.c_str(), 1, true, "offline")) {
      mqtt.publish(statusTopic.c_str(), "online", true);
      mqtt.subscribe(captureTopic.c_str());
    } else {
      delay(2000);
    }
    attempts++;
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
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  pinMode(FLASH_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  if (!initCamera()) {
    Serial.println("CAMERA INIT FAILED — halting");
    while (1) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(200);
    }
  }
  Serial.println("Camera OK");

  setupWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  reconnectMQTT();
  httpServer.begin();

  Serial.printf("HTTP stream: http://%s/stream\n", WiFi.localIP().toString().c_str());
}

// ============ LOOP ============
void loop() {
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();
  handleHttpRequest();
  delay(10);
}
