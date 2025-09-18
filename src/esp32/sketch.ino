// ESP32 Food Freshness Monitor - Firebase Integration with DHT22
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include "secrets.h"   // Create this file to store your WiFi & Firebase credentials securely

// Sensor Pin Definitions
#define DHT_PIN 25
#define DHT_TYPE DHT22
#define MQ4_PIN 34
#define MQ135_PIN 35

// Sensor Objects
DHT dht(DHT_PIN, DHT_TYPE);

// Thresholds
const float NH3_FRESH_THRESHOLD = 1.0;
const float NH3_SPOILAGE_ALERT = 50.0;
const float CH4_FERMENTATION = 50.0;
const float TEMP_MIN = 1.0;
const float TEMP_MAX = 4.0;
const float HUMIDITY_MAX = 85.0;

// Calibration values
float MQ4_R0 = 10.0;
float MQ135_R0 = 10.0;

// DHT22 tracking variables
float lastValidTemp = 25.0;
float lastValidHumidity = 50.0;
unsigned long lastDHTRead = 0;
const unsigned long DHT_READ_INTERVAL = 3000;

struct SensorReadings {
  float temperature;
  float humidity;
  float nh3_ppm;
  float ch4_ppm;
  String status;
  unsigned long timestamp;
};

SensorReadings readings;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("ESP32 Food Freshness Monitor Starting...");

  dht.begin();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());

  delay(5000);  // allow sensors to stabilize
  testDHTSensor();
  calibrateSensors();

  Serial.println("System ready! Sending data every 30s.");
}

void loop() {
  static unsigned long lastFirebaseUpdate = 0;
  if (millis() - lastFirebaseUpdate >= 30000) {
    readAllSensors();
    evaluateFoodFreshness();
    printReadings();
    sendToFirebase();
    lastFirebaseUpdate = millis();
  }
  delay(1000);
}

void testDHTSensor() {
  for (int i = 0; i < 3; i++) {
    float temp = dht.readTemperature();
    float humidity = dht.readHumidity();
    if (!isnan(temp) && !isnan(humidity)) {
      lastValidTemp = temp;
      lastValidHumidity = humidity;
    }
    delay(3000);
  }
}

void readAllSensors() {
  readDHT22();
  int mq4_raw = analogRead(MQ4_PIN);
  int mq135_raw = analogRead(MQ135_PIN);
  readings.ch4_ppm = calculateMQ4_PPM(mq4_raw);
  readings.nh3_ppm = calculateMQ135_PPM(mq135_raw);
  readings.timestamp = millis();
}

void readDHT22() {
  unsigned long currentTime = millis();
  if (currentTime - lastDHTRead >= DHT_READ_INTERVAL) {
    float temp = dht.readTemperature();
    float humidity = dht.readHumidity();
    if (!isnan(temp) && !isnan(humidity)) {
      readings.temperature = temp;
      readings.humidity = humidity;
      lastValidTemp = temp;
      lastValidHumidity = humidity;
      lastDHTRead = currentTime;
    } else {
      readings.temperature = lastValidTemp;
      readings.humidity = lastValidHumidity;
    }
  } else {
    readings.temperature = lastValidTemp;
    readings.humidity = lastValidHumidity;
  }
}

void sendToFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = FIREBASE_URL + "readings.json?auth=" + FIREBASE_SECRET;

    DynamicJsonDocument doc(512);
    doc["timestamp"] = readings.timestamp;
    doc["temperature"] = readings.temperature;
    doc["humidity"] = readings.humidity;
    doc["nh3_ppm"] = readings.nh3_ppm;
    doc["ch4_ppm"] = readings.ch4_ppm;
    doc["status"] = readings.status;
    doc["freshness_score"] = calculateFreshnessScore();
    doc["device_id"] = "food_monitor_01";

    String jsonString;
    serializeJson(doc, jsonString);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.POST(jsonString);
    http.end();
  }
}

float calculateMQ4_PPM(int raw_value) {
  if (raw_value == 0) return 0.0;
  float voltage = (raw_value / 4095.0f) * 3.3f;
  if (voltage >= 3.3f) return 0.0;
  float rs = ((3.3f - voltage) / voltage) * 10000.0f;
  float ratio = rs / MQ4_R0;
  return max(0.0f, 1000.0f * pow(ratio, -2.3f));
}

float calculateMQ135_PPM(int raw_value) {
  if (raw_value == 0) return 0.0;
  float voltage = (raw_value / 4095.0f) * 3.3f;
  if (voltage >= 3.3f) return 0.0;
  float rs = ((3.3f - voltage) / voltage) * 10000.0f;
  float ratio = rs / MQ135_R0;
  return max(0.0f, 100.0f * pow(ratio, -1.5f));
}

void evaluateFoodFreshness() {
  if (readings.nh3_ppm >= 150.0) readings.status = "SEVERE SPOILAGE";
  else if (readings.nh3_ppm >= NH3_SPOILAGE_ALERT) readings.status = "SPOILAGE DETECTED";
  else if (readings.nh3_ppm >= 10.0) readings.status = "EARLY SPOILAGE WARNING";
  else if (readings.nh3_ppm >= NH3_FRESH_THRESHOLD) readings.status = "SLIGHTLY ELEVATED";
  else readings.status = "FRESH";

  if (readings.ch4_ppm >= CH4_FERMENTATION) readings.status += " - FERMENTATION";
  if (readings.temperature < TEMP_MIN || readings.temperature > TEMP_MAX) readings.status += " - TEMP ALERT";
  if (readings.humidity > HUMIDITY_MAX) readings.status += " - HIGH HUMIDITY";
}

void calibrateSensors() {
  float mq4_sum = 0, mq135_sum = 0;
  int samples = 50;
  for (int i = 0; i < samples; i++) {
    mq4_sum += analogRead(MQ4_PIN);
    mq135_sum += analogRead(MQ135_PIN);
    delay(100);
  }
  float mq4_avg = mq4_sum / samples;
  float mq135_avg = mq135_sum / samples;
  float mq4_voltage = (mq4_avg / 4095.0f) * 3.3f;
  float mq135_voltage = (mq135_avg / 4095.0f) * 3.3f;
  MQ4_R0 = ((3.3f - mq4_voltage) / mq4_voltage) * 10000.0f;
  MQ135_R0 = ((3.3f - mq135_voltage) / mq135_voltage) * 10000.0f;
}

int calculateFreshnessScore() {
  int score = 100;
  if (readings.nh3_ppm >= 150.0f) score -= 70;
  else if (readings.nh3_ppm >= NH3_SPOILAGE_ALERT) score -= 50;
  else if (readings.nh3_ppm >= 10.0f) score -= 30;
  else if (readings.nh3_ppm >= NH3_FRESH_THRESHOLD) score -= 15;
  if (readings.ch4_ppm >= CH4_FERMENTATION) score -= 25;
  if (readings.temperature < TEMP_MIN || readings.temperature > TEMP_MAX) score -= 20;
  if (readings.humidity > HUMIDITY_MAX) score -= 15;
  return max(0, min(100, score));
}

void printReadings() {
  Serial.println("=== Food Freshness Monitor ===");
  Serial.printf("Temperature: %.2f°C\n", readings.temperature);
  Serial.printf("Humidity: %.2f%%\n", readings.humidity);
  Serial.printf("NH3: %.2f ppm\n", readings.nh3_ppm);
  Serial.printf("CH4: %.2f ppm\n", readings.ch4_ppm);
  Serial.printf("Status: %s\n", readings.status.c_str());
  Serial.printf("Freshness Score: %d/100\n", calculateFreshnessScore());
  Serial.printf("Timestamp: %lu\n", readings.timestamp);
  Serial.println("==============================");
}
