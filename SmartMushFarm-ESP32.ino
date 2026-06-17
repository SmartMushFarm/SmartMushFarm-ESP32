#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "Adafruit_SHT31.h"
#include <ArduinoJson.h>

// ===== DEVICE CONFIG =====
const char* deviceId = "esp32_001";

// ===== MQTT EMQX CONFIG =====
const char* mqtt_server = "xe1572fa.ala.asia-southeast1.emqxsl.com";
const int mqtt_port = 8883;

const char* mqtt_user = "admin";
const char* mqtt_pass = "12345678";

// ===== MQTT TOPIC =====
String sensorTopic  = "mushroom/esp32_001/sensor";
String statusTopic  = "mushroom/esp32_001/status";
String commandTopic = "mushroom/esp32_001/command";

// ===== CLIENTS =====
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// ===== SHT30 =====
Adafruit_SHT31 sht30 = Adafruit_SHT31();
bool sht30Ok = false;

// ===== MOSFET PINS =====
// 1 MOSFET điều khiển 2 quạt
// 1 MOSFET điều khiển 2 sưởi
// 1 MOSFET điều khiển 2 phun sương
// 1 MOSFET điều khiển đèn
const int FAN_PIN = 25;
const int HEATER_PIN = 26;
const int MIST_PIN = 27;
const int LIGHT_PIN = 33;

// Nếu MOSFET module bật bằng HIGH thì để true.
// Nếu module của mày bật bằng LOW thì đổi thành false.
const bool ACTIVE_HIGH = true;

// ===== DEVICE STATUS =====
bool fanStatus = false;
bool heaterStatus = false;
bool mistStatus = false;
bool lightStatus = false;

// ===== TIMER =====
unsigned long lastSend = 0;
const unsigned long sendInterval = 5000;

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 5000;

unsigned long lastMqttReconnect = 0;
const unsigned long mqttReconnectInterval = 3000;

// ===== CONTROL OUTPUT =====
void writeOutput(int pin, bool state) {
  if (ACTIVE_HIGH) {
    digitalWrite(pin, state ? HIGH : LOW);
  } else {
    digitalWrite(pin, state ? LOW : HIGH);
  }
}

void setFan(bool state) {
  writeOutput(FAN_PIN, state);
  fanStatus = state;
}

void setHeater(bool state) {
  writeOutput(HEATER_PIN, state);
  heaterStatus = state;
}

void setMist(bool state) {
  writeOutput(MIST_PIN, state);
  mistStatus = state;
}

void setLight(bool state) {
  writeOutput(LIGHT_PIN, state);
  lightStatus = state;
}

void turnAllOff() {
  setFan(false);
  setHeater(false);
  setMist(false);
  setLight(false);
}

void turnAllOn() {
  setFan(true);
  setHeater(true);
  setMist(true);
  setLight(true);
}

// ===== WIFI SETUP =====
void setupWiFi() {
  Serial.println();
  Serial.println("===== WIFI SETUP =====");

  WiFiManager wm;

  wm.setConnectTimeout(60);
  wm.setConfigPortalTimeout(300);

  // Nếu muốn xóa WiFi cũ, mở dòng này upload 1 lần
  // wm.resetSettings();

  bool connected = wm.autoConnect("Mushroom_Setup");

  if (!connected) {
    Serial.println("WiFi connect failed or config portal timeout.");
    Serial.println("Restarting ESP32...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("WiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ===== WIFI RECONNECT =====
void checkWiFi() {
  if (millis() - lastWifiCheck < wifiCheckInterval) {
    return;
  }

  lastWifiCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println("WiFi lost. Trying to reconnect...");

  WiFi.reconnect();

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi reconnected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi reconnect failed. Restarting...");
    delay(1000);
    ESP.restart();
  }
}

// ===== MQTT CALLBACK: NHẬN LỆNH TỪ BACKEND =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");

  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.println(message);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.println("JSON parse failed");
    return;
  }

  String device = doc["device"] | "";
  String action = doc["action"] | "";

  bool state;

  if (action == "on") {
    state = true;
  } else if (action == "off") {
    state = false;
  } else {
    Serial.println("Invalid action");
    return;
  }

  if (device == "fan") {
    setFan(state);
  } 
  else if (device == "heater") {
    setHeater(state);
  } 
  else if (device == "mist") {
    setMist(state);
  }
  else if (device == "light") {
    setLight(state);
  }
  else if (device == "all") {
    if (state) {
      turnAllOn();
    } else {
      turnAllOff();
    }
  } 
  else {
    Serial.println("Unknown device");
    return;
  }

  publishStatus("COMMAND_EXECUTED");
}

// ===== MQTT RECONNECT =====
void reconnectMQTT() {
  if (mqttClient.connected()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastMqttReconnect < mqttReconnectInterval) {
    return;
  }

  lastMqttReconnect = millis();

  Serial.print("Connecting MQTT... ");

  String clientId = "ESP32Client-";
  clientId += deviceId;
  clientId += "-";
  clientId += String(random(0xffff), HEX);

  if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
    Serial.println("connected");

    mqttClient.subscribe(commandTopic.c_str());

    Serial.print("Subscribed command topic: ");
    Serial.println(commandTopic);

    publishStatus("MQTT_CONNECTED");
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" will retry later");
  }
}

// ===== PUBLISH STATUS =====
void publishStatus(String eventType) {
  if (!mqttClient.connected()) {
    return;
  }

  StaticJsonDocument<512> doc;

  doc["deviceId"] = deviceId;
  doc["eventType"] = eventType;
  doc["wifiIp"] = WiFi.localIP().toString();

  JsonObject output = doc.createNestedObject("outputStatus");
  output["fan"] = fanStatus;
  output["heater"] = heaterStatus;
  output["mist"] = mistStatus;
  output["light"] = lightStatus;

  char buffer[512];
  serializeJson(doc, buffer);

  mqttClient.publish(statusTopic.c_str(), buffer);

  Serial.println("Status published:");
  Serial.println(buffer);
}

// ===== PUBLISH SENSOR =====
void publishSensorData() {
  if (!sht30Ok) {
    Serial.println("Skip publish: SHT30 is not available.");
    return;
  }

  float temp = sht30.readTemperature();
  float humidity = sht30.readHumidity();

  if (isnan(temp) || isnan(humidity)) {
    Serial.println("Failed to read SHT30");
    return;
  }

  StaticJsonDocument<512> doc;

  doc["deviceId"] = deviceId;
  doc["temperature"] = temp;
  doc["humidity"] = humidity;

  JsonObject output = doc.createNestedObject("outputStatus");
  output["fan"] = fanStatus;
  output["heater"] = heaterStatus;
  output["mist"] = mistStatus;
  output["light"] = lightStatus;

  char buffer[512];
  serializeJson(doc, buffer);

  Serial.println("Publishing:");
  Serial.println(buffer);

  if (mqttClient.connected()) {
    bool ok = mqttClient.publish(sensorTopic.c_str(), buffer);

    if (ok) {
      Serial.println("Publish success");
    } else {
      Serial.println("Publish failed");
    }
  } else {
    Serial.println("MQTT not connected. Skip publish.");
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32 BOOTING...");
  Serial.println("Device ID: esp32_001");
  Serial.println("================================");

  // ===== MOSFET PIN SETUP =====
  pinMode(FAN_PIN, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  pinMode(MIST_PIN, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT);

  // Khi khởi động thì tắt hết để an toàn
  turnAllOff();

  // ===== I2C SHT30 =====
  Wire.begin(21, 22);

  if (!sht30.begin(0x44)) {
    Serial.println("SHT30 not found. Check wiring!");
    Serial.println("ESP32 will continue running without sensor.");
    sht30Ok = false;
  } else {
    Serial.println("SHT30 found!");
    sht30Ok = true;
  }

  // ===== WIFI MANAGER =====
  setupWiFi();

  // ===== MQTT TLS =====
  espClient.setInsecure();

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  Serial.println("Setup done.");
}

// ===== LOOP =====
void loop() {
  checkWiFi();

  reconnectMQTT();

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  if (millis() - lastSend >= sendInterval) {
    lastSend = millis();
    publishSensorData();
  }
}