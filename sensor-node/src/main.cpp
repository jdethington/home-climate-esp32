/*
 * Home Climate - Sensor Node
 * Reads DHT11 and POSTs JSON to the central web-server ESP32.
 *
 * Configure:
 *   1. Copy shared/secrets.example.h → src/secrets.h and edit
 *   2. Set NODE_ID and LOCATION below (or via build flags)
 *   3. Build & upload with PlatformIO
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

#include "config.h"
#include "secrets.h"

// ---------- Per-node identity ----------
// Prefer the PlatformIO environments (indoor1, outdoor2, …)
#ifndef NODE_ID
#define NODE_ID       "in1"
#endif
#ifndef LOCATION
#define LOCATION      "indoor"
#endif
// --------------------------------------

DHT dht(DHT_PIN, DHT_TYPE);

unsigned long lastSend = 0;

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // more reliable on some routers
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi OK  IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\nWiFi failed – will retry later");
  }
}

bool sendReadingOnce(float tempC, float humidity) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return false;
  }

  HTTPClient http;
  http.setConnectTimeout(5000);   // 5 s to connect
  http.setTimeout(8000);          // 8 s overall

  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + API_PATH;
  if (!http.begin(url)) {
    Serial.println("http.begin() failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  JsonDocument doc;
  doc["id"]       = NODE_ID;
  doc["location"] = LOCATION;
  doc["temp_c"]   = serialized(String(tempC, 1));
  doc["humidity"] = serialized(String(humidity, 1));
  doc["rssi"]     = WiFi.RSSI();
  doc["uptime_s"] = millis() / 1000;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  bool ok = (code >= 200 && code < 300);

  Serial.printf("POST %s → HTTP %d  (%s)\n", url.c_str(), code, ok ? "OK" : "FAIL");
  if (!ok && code > 0) {
    Serial.println(http.getString());
  }

  http.end();
  return ok;
}

bool sendReading(float tempC, float humidity) {
  // Retry up to 3 times – home Wi-Fi between ESP32s can be flaky
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (sendReadingOnce(tempC, humidity)) return true;
    Serial.printf("  retry %d/3...\n", attempt);
    delay(1000 * attempt);   // 1 s, 2 s, 3 s backoff
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Climate Sensor Node ===");
  Serial.printf("ID: %s  Location: %s\n", NODE_ID, LOCATION);

  dht.begin();
  delay(2000);                 // DHT11 needs a moment after power-up
  connectWiFi();
}

void loop() {
  unsigned long now = millis();
  if (now - lastSend < SENSOR_INTERVAL_MS) {
    delay(200);
    return;
  }
  lastSend = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();   // Celsius

  if (isnan(h) || isnan(t)) {
    Serial.println("DHT11 read failed – check wiring");
    return;
  }

  // Skip obviously bad first readings (common right after boot)
  if (t < 1.0 && h < 1.0) {
    Serial.println("DHT11 still warming up – skipping");
    return;
  }

  Serial.printf("Temp: %.1f °C  Humidity: %.1f %%  RSSI: %d\n", t, h, WiFi.RSSI());
  sendReading(t, h);
}
