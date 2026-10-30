/*
 * Home Climate - Web Server / Hub
 * Receives readings from sensor nodes and serves a live dashboard
 * with an "Open windows?" recommendation + status LEDs + local TMP36.
 *
 * LED wiring (common-cathode RGB):
 *   Wi-Fi / browser activity  → GPIO 16 + 220–330Ω → GND
 *   Sensor POST received      → GPIO 2  + 220–330Ω → GND  (onboard LED)
 *   RGB Red                   → GPIO 13 + 220–330Ω ─┐
 *   RGB Green                 → GPIO 14 + 220–330Ω ─┼─ common cathode → GND
 *   RGB Blue                  → GPIO 18 + 220–330Ω ─┘
 *   Indoor data fresh         → GPIO 19 + 220–330Ω → GND
 *   Outdoor data fresh        → GPIO 21 + 220–330Ω → GND
 *
 * Pin choices avoid the ESP32 boot strapping pins (0, 5, 12, 15).
 * GPIO 2 is kept because it drives the onboard LED.
 *
 * TMP36 (Arduino Starter Kit):
 *   Vs   → 3.3V
 *   Vout → GPIO 34
 *   GND  → GND
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

#include "config.h"
#include "secrets.h"

// ---------- LED pins ----------
#define LED_WIFI      16
#define LED_RX        2    // onboard LED
#define LED_RGB_R     13
#define LED_RGB_G     14
#define LED_RGB_B     18
#define LED_INDOOR    19
#define LED_OUTDOOR   21

// ---------- TMP36 ----------
#define TMP36_PIN     34
#define TMP36_SAMPLES 16
#define TMP36_OFFSET_C -4.0f   // adjust as needed to calibrate the TMP36 sensor

#define FRESH_MS      180000UL

#define RGB_FREQ      5000
#define RGB_RES       8

struct NodeReading {
  char id[16];
  char location[12];
  float tempHist[READING_HISTORY_N];  // rolling window of raw readings
  float humHist[READING_HISTORY_N];
  uint8_t histIdx;
  uint8_t histCount;
  float temp_c;        // rolling mean over the window — what the hub decides on
  float humidity;      // rolling mean over the window
  int rssi;
  unsigned long last_seen_ms;
  bool valid;
};

// Set by getAverages(): true when indoor temp fell back to the hub's TMP36
// because no indoor node is reporting.
bool indoorFromHub = false;

NodeReading nodes[MAX_NODES];
int nodeCount = 0;

WebServer server(80);

unsigned long rxBlinkUntil   = 0;
unsigned long wifiBlinkUntil = 0;

float localTempC = NAN;
unsigned long lastTmp36Read = 0;

float cToF(float c) { return c * 9.0f / 5.0f + 32.0f; }

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(LED_RGB_R, r);
  ledcWrite(LED_RGB_G, g);
  ledcWrite(LED_RGB_B, b);
}

float readTmp36C() {
  uint32_t sumMv = 0;
  for (int i = 0; i < TMP36_SAMPLES; i++) {
    sumMv += analogReadMilliVolts(TMP36_PIN);
    delay(2);
  }
  float voltage = (sumMv / (float)TMP36_SAMPLES) / 1000.0f;
  return (voltage - 0.5f) * 100.0f + TMP36_OFFSET_C;
}

void getAverages(float& indoorTemp, float& indoorHum,
                 float& outdoorTemp, float& outdoorHum,
                 int& indoorCount, int& outdoorCount);
void updateStatusLeds();

enum RecState { REC_WAITING, REC_OPEN, REC_CAUTION, REC_MARGINAL, REC_CLOSED };
RecState computeRecommendation(float iT, float iH, float oT, float oH,
                               int iCnt, int oCnt);

NodeReading* findOrCreate(const char* id) {
  for (int i = 0; i < nodeCount; i++) {
    if (strcmp(nodes[i].id, id) == 0) return &nodes[i];
  }
  if (nodeCount >= MAX_NODES) return nullptr;
  NodeReading* n = &nodes[nodeCount++];
  strncpy(n->id, id, sizeof(n->id) - 1);
  n->id[sizeof(n->id) - 1] = '\0';
  n->valid = false;
  return n;
}

// Push one raw reading into the node's rolling window and refresh its
// smoothed (mean) values. Window length is READING_HISTORY_N (≈5 min).
void pushNodeReading(NodeReading* n, float tempC, float humidity) {
  n->tempHist[n->histIdx] = tempC;
  n->humHist[n->histIdx]  = humidity;
  n->histIdx = (n->histIdx + 1) % READING_HISTORY_N;
  if (n->histCount < READING_HISTORY_N) n->histCount++;

  float sumT = 0, sumH = 0;
  for (uint8_t i = 0; i < n->histCount; i++) {
    sumT += n->tempHist[i];
    sumH += n->humHist[i];
  }
  n->temp_c   = sumT / n->histCount;
  n->humidity = sumH / n->histCount;
}

void getAverages(float& indoorTemp, float& indoorHum,
                 float& outdoorTemp, float& outdoorHum,
                 int& indoorCount, int& outdoorCount) {
  indoorTemp = indoorHum = outdoorTemp = outdoorHum = 0;
  indoorCount = outdoorCount = 0;

  unsigned long now = millis();
  for (int i = 0; i < nodeCount; i++) {
    if (!nodes[i].valid) continue;
    if (now - nodes[i].last_seen_ms > FRESH_MS) continue;

    if (strcmp(nodes[i].location, "outdoor") == 0) {
      outdoorTemp += nodes[i].temp_c;
      outdoorHum  += nodes[i].humidity;
      outdoorCount++;
    } else {
      indoorTemp += nodes[i].temp_c;
      indoorHum  += nodes[i].humidity;
      indoorCount++;
    }
  }
  if (indoorCount)  { indoorTemp  /= indoorCount;  indoorHum  /= indoorCount; }

  // Fallback: no indoor node reporting — use the hub's own TMP36 as the
  // indoor temperature. Humidity is unknown in this case (NaN → JSON null).
  indoorFromHub = false;
  if (indoorCount == 0 && !isnan(localTempC)) {
    indoorTemp    = localTempC;
    indoorHum     = NAN;
    indoorCount   = 1;
    indoorFromHub = true;
  }

  if (outdoorCount) { outdoorTemp /= outdoorCount; outdoorHum /= outdoorCount; }
}

void updateStatusLeds() {
  float iT, iH, oT, oH;
  int iCnt, oCnt;
  getAverages(iT, iH, oT, oH, iCnt, oCnt);

  unsigned long now = millis();
  bool indoorFresh = false;
  bool outdoorFresh = false;
  for (int i = 0; i < nodeCount; i++) {
    if (!nodes[i].valid) continue;
    if (now - nodes[i].last_seen_ms > FRESH_MS) continue;
    if (strcmp(nodes[i].location, "outdoor") == 0) outdoorFresh = true;
    else indoorFresh = true;
  }
  digitalWrite(LED_INDOOR,  indoorFresh  ? HIGH : LOW);
  digitalWrite(LED_OUTDOOR, outdoorFresh ? HIGH : LOW);

  switch (computeRecommendation(iT, iH, oT, oH, iCnt, oCnt)) {
    case REC_OPEN:     setRgb(0, 180, 0);   break;  // green
    case REC_CAUTION:  setRgb(220, 120, 0); break;  // amber
    case REC_MARGINAL: setRgb(180, 100, 0); break;  // dim orange
    case REC_CLOSED:   setRgb(200, 0, 0);   break;  // red
    default:           setRgb(0, 0, 0);     break;  // off = waiting for data
  }

  if (wifiBlinkUntil == 0) {
    digitalWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
  }
}

// Hysteresis state machine: latch OPEN once outdoor is cooler by
// TEMP_DELTA_OPEN_C, hold it until the margin shrinks below
// TEMP_DELTA_CLOSE_C (or outdoor humidity goes bad). Both the RGB LED
// and the dashboard text go through here so they can never disagree.
RecState computeRecommendation(float iT, float iH, float oT, float oH,
                               int iCnt, int oCnt) {
  (void)iH;  // indoor humidity is display-only; the gate is outdoor humidity
  static bool openLatched = false;

  if (iCnt == 0 || oCnt == 0) { openLatched = false; return REC_WAITING; }

  bool humidityOk = (oH <= MAX_OUTDOOR_RH);
  bool coolToOpen = (oT <= iT - TEMP_DELTA_OPEN_C);
  bool coolToHold = (oT <= iT - TEMP_DELTA_CLOSE_C);

  if (!openLatched) {
    if (coolToOpen && humidityOk) openLatched = true;
  } else if (!coolToHold || !humidityOk) {
    openLatched = false;
  }

  if (openLatched)                return REC_OPEN;
  if (coolToOpen && !humidityOk)   return REC_CAUTION;
  if (oT < iT)                    return REC_MARGINAL;
  return REC_CLOSED;
}

const char* recStateText(RecState s) {
  switch (s) {
    case REC_OPEN:     return "✅ OPEN windows / doors — outdoor air is cooler and humidity is acceptable";
    case REC_CAUTION:  return "⚠️ Cooler outside but humidity is high — open only if you accept more moisture";
    case REC_MARGINAL: return "➖ Slightly cooler outside — marginal benefit";
    case REC_CLOSED:   return "❌ Keep closed — outdoor is warmer or not beneficial";
    default:           return "Waiting for indoor & outdoor data…";
  }
}

const char* getRecommendation(float iT, float iH, float oT, float oH,
                              int iCnt, int oCnt) {
  return recStateText(computeRecommendation(iT, iH, oT, oH, iCnt, oCnt));
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Home Climate</title>
<style>
  :root { --bg:#0f172a; --card:#1e293b; --text:#e2e8f0; --accent:#38bdf8; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { font-family:system-ui,-apple-system,sans-serif; background:var(--bg); color:var(--text); min-height:100vh; padding:1rem; }
  h1 { text-align:center; margin-bottom:0.5rem; font-size:1.6rem; }
  .sub { text-align:center; color:#94a3b8; margin-bottom:1rem; font-size:0.9rem; }
  .toolbar { display:flex; justify-content:center; margin-bottom:1.25rem; }
  .unit-toggle {
    display:inline-flex; align-items:center; gap:0.5rem;
    background:var(--card); border:1px solid #334155; border-radius:999px;
    padding:0.35rem 0.75rem; font-size:0.9rem; user-select:none;
  }
  .unit-toggle span { color:#94a3b8; }
  .unit-toggle span.active { color:var(--accent); font-weight:600; }
  .unit-toggle input { appearance:none; width:2.4rem; height:1.3rem; border-radius:999px;
    background:#334155; position:relative; cursor:pointer; outline:none; }
  .unit-toggle input::after {
    content:''; position:absolute; top:2px; left:2px; width:1rem; height:1rem;
    border-radius:50%; background:#e2e8f0; transition:transform 0.15s; }
  .unit-toggle input:checked { background:var(--accent); }
  .unit-toggle input:checked::after { transform:translateX(1.1rem); }
  .rec { background:var(--card); border-radius:12px; padding:1.2rem; margin-bottom:1.5rem; text-align:center; font-size:1.15rem; line-height:1.4; border:2px solid #334155; }
  .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(260px,1fr)); gap:1rem; }
  .card { background:var(--card); border-radius:12px; padding:1rem; }
  .card h2 { font-size:1rem; color:var(--accent); margin-bottom:0.6rem; }
  .row { display:flex; justify-content:space-between; padding:0.35rem 0; border-bottom:1px solid #334155; }
  .row:last-child { border-bottom:none; }
  .label { color:#94a3b8; }
  .val { font-weight:600; }
  .stale { opacity:0.45; }
  footer { text-align:center; margin-top:2rem; color:#64748b; font-size:0.8rem; }
</style>
</head>
<body>
  <h1>🏠 Home Climate</h1>
  <p class="sub">Live indoor vs outdoor comparison</p>

  <div class="toolbar">
    <label class="unit-toggle">
      <span id="labelC" class="active">°C</span>
      <input type="checkbox" id="unitSwitch" aria-label="Toggle Fahrenheit">
      <span id="labelF">°F</span>
    </label>
  </div>

  <div class="rec" id="recommendation">Loading…</div>
  <div class="grid" id="cards"></div>
  <footer>Auto-refreshes every 15 s · Node values are 5-min rolling averages · ESP32 Climate Hub</footer>

<script>
let useF = localStorage.getItem('climateUnit') === 'F';
const unitSwitch = document.getElementById('unitSwitch');
const labelC = document.getElementById('labelC');
const labelF = document.getElementById('labelF');

function applyUnitUI() {
  unitSwitch.checked = useF;
  labelC.classList.toggle('active', !useF);
  labelF.classList.toggle('active', useF);
}

function fmtTemp(c, f) {
  if (useF) return f.toFixed(1) + ' °F';
  return c.toFixed(1) + ' °C';
}

unitSwitch.addEventListener('change', () => {
  useF = unitSwitch.checked;
  localStorage.setItem('climateUnit', useF ? 'F' : 'C');
  applyUnitUI();
  refresh();
});
applyUnitUI();

async function refresh() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('recommendation').textContent = d.recommendation;
    const cards = document.getElementById('cards');
    cards.innerHTML = '';

    const sum = document.createElement('div');
    sum.className = 'card';
    let localRow = '';
    if (d.local && d.local.temp_c != null && !isNaN(d.local.temp_c)) {
      localRow = `<div class="row"><span class="label">Hub (TMP36)</span><span class="val">${fmtTemp(d.local.temp_c, d.local.temp_f)}</span></div>`;
    }
    sum.innerHTML = `
      <h2>Averages</h2>
      ${localRow}
      <div class="row"><span class="label">Indoor Temp${d.indoor.source === 'hub_tmp36' ? ' (hub TMP36)' : ''}</span><span class="val">${fmtTemp(d.indoor.temp_c, d.indoor.temp_f)}</span></div>
      <div class="row"><span class="label">Indoor Humidity</span><span class="val">${d.indoor.humidity == null ? '—' : d.indoor.humidity.toFixed(0) + ' %'}</span></div>
      <div class="row"><span class="label">Outdoor Temp</span><span class="val">${fmtTemp(d.outdoor.temp_c, d.outdoor.temp_f)}</span></div>
      <div class="row"><span class="label">Outdoor Humidity</span><span class="val">${d.outdoor.humidity.toFixed(0)} %</span></div>
      <div class="row"><span class="label">Nodes reporting</span><span class="val">${d.indoor.count} in / ${d.outdoor.count} out</span></div>
    `;
    cards.appendChild(sum);

    d.nodes.forEach(n => {
      const age = Math.round(n.age_s);
      const stale = age > 120;
      const el = document.createElement('div');
      el.className = 'card' + (stale ? ' stale' : '');
      el.innerHTML = `
        <h2>${n.id} <small style="color:#94a3b8">(${n.location})</small></h2>
        <div class="row"><span class="label">Temperature</span><span class="val">${fmtTemp(n.temp_c, n.temp_f)}</span></div>
        <div class="row"><span class="label">Humidity</span><span class="val">${n.humidity.toFixed(0)} %</span></div>
        <div class="row"><span class="label">RSSI</span><span class="val">${n.rssi} dBm</span></div>
        <div class="row"><span class="label">Last seen</span><span class="val">${age}s ago</span></div>
      `;
      cards.appendChild(el);
    });
  } catch (e) {
    document.getElementById('recommendation').textContent = 'Error loading data';
  }
}
refresh();
setInterval(refresh, 15000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  wifiBlinkUntil = millis() + 120;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  wifiBlinkUntil = millis() + 80;

  float iT, iH, oT, oH;
  int iCnt, oCnt;
  getAverages(iT, iH, oT, oH, iCnt, oCnt);

  JsonDocument doc;
  doc["recommendation"] = getRecommendation(iT, iH, oT, oH, iCnt, oCnt);

  JsonObject local = doc["local"].to<JsonObject>();
  if (!isnan(localTempC)) {
    local["temp_c"] = localTempC;
    local["temp_f"] = cToF(localTempC);
  } else {
    local["temp_c"] = nullptr;
    local["temp_f"] = nullptr;
  }

  JsonObject indoor = doc["indoor"].to<JsonObject>();
  indoor["temp_c"]   = iT;
  indoor["temp_f"]   = cToF(iT);
  indoor["humidity"] = iH;   // null when indoor temp fell back to the hub TMP36
  indoor["count"]    = iCnt;
  indoor["source"]   = indoorFromHub ? "hub_tmp36" : "nodes";

  JsonObject outdoor = doc["outdoor"].to<JsonObject>();
  outdoor["temp_c"]   = oT;
  outdoor["temp_f"]   = cToF(oT);
  outdoor["humidity"] = oH;
  outdoor["count"]    = oCnt;

  JsonArray arr = doc["nodes"].to<JsonArray>();
  unsigned long now = millis();
  for (int i = 0; i < nodeCount; i++) {
    if (!nodes[i].valid) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"]       = nodes[i].id;
    o["location"] = nodes[i].location;
    o["temp_c"]   = nodes[i].temp_c;
    o["temp_f"]   = cToF(nodes[i].temp_c);
    o["humidity"] = nodes[i].humidity;
    o["rssi"]     = nodes[i].rssi;
    o["age_s"]    = (now - nodes[i].last_seen_ms) / 1000UL;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleReading() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"POST only\"}");
    return;
  }

  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  const char* id = doc["id"] | "unknown";
  NodeReading* n = findOrCreate(id);
  if (!n) {
    server.send(503, "application/json", "{\"error\":\"too many nodes\"}");
    return;
  }

  strncpy(n->location, doc["location"] | "indoor", sizeof(n->location) - 1);
  n->location[sizeof(n->location) - 1] = '\0';
  pushNodeReading(n, doc["temp_c"] | 0.0f, doc["humidity"] | 0.0f);
  n->rssi         = doc["rssi"] | 0;
  n->last_seen_ms = millis();
  n->valid        = true;

  rxBlinkUntil = millis() + 150;
  Serial.printf("RX %s (%s): %.1f°C  %.0f%%  (smoothed %.1f°C / %d samples)\n",
                n->id, n->location, (double)(doc["temp_c"] | 0.0f),
                (double)(doc["humidity"] | 0.0f), n->temp_c, n->histCount);

  server.send(200, "application/json", "{\"status\":\"ok\"}");
  updateStatusLeds();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void connectWiFi() {
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
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
    Serial.println(">>> Set SERVER_HOST in sensor secrets.h to the IP above <<<");
    digitalWrite(LED_WIFI, HIGH);
  } else {
    Serial.println("\nWiFi failed – starting soft AP for setup");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID_FALLBACK, AP_PASS_FALLBACK);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    digitalWrite(LED_WIFI, LOW);
  }
}

void setupLeds() {
  pinMode(LED_WIFI,    OUTPUT);
  pinMode(LED_RX,      OUTPUT);
  pinMode(LED_INDOOR,  OUTPUT);
  pinMode(LED_OUTDOOR, OUTPUT);

  ledcAttach(LED_RGB_R, RGB_FREQ, RGB_RES);
  ledcAttach(LED_RGB_G, RGB_FREQ, RGB_RES);
  ledcAttach(LED_RGB_B, RGB_FREQ, RGB_RES);

  digitalWrite(LED_WIFI,    LOW);
  digitalWrite(LED_RX,      LOW);
  digitalWrite(LED_INDOOR,  LOW);
  digitalWrite(LED_OUTDOOR, LOW);
  setRgb(0, 0, 0);
}

void testRgbCycle() {
  int delayTime = 500;
  int pwmDelay  = 10;
  int offDelay  = 300;
  int repeat    = 1;
  int j         = 0;

  Serial.println("RGB test: RED, GREEN, BLUE, YELLOW, WHITE, OFF");
  for (int n = 0; n < repeat; n++) {
    Serial.printf("Test %d\n", n + 1);
    setRgb(255, 0, 0); delay(delayTime);
    setRgb(0, 255, 0); delay(delayTime);
    setRgb(0, 0, 255); delay(delayTime);
    setRgb(255, 160, 0); delay(delayTime);
    setRgb(200, 200, 200); delay(delayTime);
    setRgb(0, 0, 0); delay(offDelay);
  }

  Serial.println("RGB test: fade green");
  for (int i = 0; i <= 180; i += 5) { setRgb(0, i, 0); delay(pwmDelay); }
  for (int i = 180; i >= 0; i -= 5) { setRgb(0, i, 0); delay(pwmDelay); }

  Serial.println("RGB test: fade yellow/amber");
  for (int i = 0; i <= 200; i += 5) {
    j = (i * 120) / 200;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }
  for (int i = 200; i >= 0; i -= 5) {
    j = (i * 120) / 200;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }

  setRgb(0, 0, 0);
  delay(offDelay);

  Serial.println("RGB test: fade softer yellow");
  for (int i = 0; i <= 180; i += 5) {
    j = (i * 100) / 180;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }
  for (int i = 180; i >= 0; i -= 5) {
    j = (i * 100) / 180;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }

  Serial.println("RGB test: fade red");
  for (int i = 0; i <= 255; i += 5) { setRgb(i, 0, 0); delay(pwmDelay); }
  for (int i = 255; i >= 0; i -= 5) { setRgb(i, 0, 0); delay(pwmDelay); }

  Serial.println("RGB test: fade white");
  for (int i = 0; i <= 255; i += 5) { setRgb(i, i, i); delay(pwmDelay); }
  for (int i = 255; i >= 0; i -= 5) { setRgb(i, i, i); delay(pwmDelay); }

  Serial.println("RGB test: OFF");
  setRgb(0, 0, 0);
  delay(offDelay);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Climate Web Server ===");

  analogSetPinAttenuation(TMP36_PIN, ADC_11db);

  setupLeds();
  testRgbCycle();

  localTempC = readTmp36C();
  Serial.printf("TMP36 local: %.1f °C / %.1f °F\n", localTempC, cToF(localTempC));
  lastTmp36Read = millis();

  memset(nodes, 0, sizeof(nodes));
  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reading", HTTP_POST, handleReading);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
  updateStatusLeds();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  digitalWrite(LED_RX, (now < rxBlinkUntil) ? HIGH : LOW);

  if (now < wifiBlinkUntil) {
    digitalWrite(LED_WIFI, LOW);
  } else if (wifiBlinkUntil != 0 && now >= wifiBlinkUntil) {
    wifiBlinkUntil = 0;
    digitalWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
  }

  if (now - lastTmp36Read > 10000UL) {
    lastTmp36Read = now;
    localTempC = readTmp36C();
    Serial.printf("TMP36(Hub Temp): %.1f °C / %.1f °F\n", localTempC, cToF(localTempC));
  }

  static unsigned long lastLedUpdate = 0;
  if (now - lastLedUpdate > 2000) {
    lastLedUpdate = now;
    updateStatusLeds();
  }
}
