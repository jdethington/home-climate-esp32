/*
 * Home Climate - Web Server / Hub
 * Receives readings from sensor nodes and serves a live dashboard
 * with an "Open windows?" recommendation + status LEDs.
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
 * DHT11/22 sensor wiring:
 *   VCC   → 3.3V
 *   Vout → GPIO 4
 *   GND  → GND
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>

#include "config.h"
#include "secrets.h"

// ---------- LED pins ----------
#define LED_WIFI 16
#define LED_RX 2 // onboard LED — active-HIGH on this board (HIGH = on, verified)
#define LED_RGB_R 13
#define LED_RGB_G 14
#define LED_RGB_B 18
#define LED_INDOOR 19
#define LED_OUTDOOR 21

// ---------- DHT Sensor 11/22 ----------
DHT dht22(DHT_PIN, DHT22); // candidate drivers; detectSensor() picks one
DHT dht11(DHT_PIN, DHT11);
DHT *dht = nullptr; // active driver after auto-detect
unsigned int hubReadFailures = 0;

#define FRESH_MS 180000UL

#define RGB_FREQ 5000
#define RGB_RES 8

struct NodeReading
{
  char id[16];
  char location[12];
  float tempHist[READING_HISTORY_N]; // rolling window of raw readings
  float humHist[READING_HISTORY_N];
  uint8_t histIdx;
  uint8_t histCount;
  float temp_c;   // rolling mean over the window — what the hub decides on
  float humidity; // rolling mean over the window
  int rssi;
  unsigned long last_seen_ms;
  bool valid;
};

NodeReading nodes[MAX_NODES];
int nodeCount = 0;

WebServer server(80);

unsigned long rxBlinkUntil = 0;
unsigned long wifiBlinkUntil = 0;

// A DHT11 misread as DHT22 yields impossible values (tenths x256 scale:
// room temp decodes to 500+ °C), while a DHT22 misread as DHT11 can look
// plausible (e.g. 1.5 °C). So try DHT22 first: whichever interpretation
// lands in sane ranges identifies the real sensor.
bool saneReading(float t, float h)
{
  return !isnan(t) && !isnan(h) && t > -40.0f && t < 80.0f && h >= 0.0f && h <= 100.0f;
}

void detectSensor()
{
  dht22.begin();
  delay(2000); // both sensors need a moment after power-up
  if (saneReading(dht22.readTemperature(), dht22.readHumidity()))
  {
    dht = &dht22;
    Serial.println("Sensor auto-detect: DHT22");
    return;
  }
  dht11.begin();
  delay(2000);
  if (saneReading(dht11.readTemperature(), dht11.readHumidity()))
  {
    dht = &dht11;
    Serial.println("Sensor auto-detect: DHT11");
    return;
  }
  Serial.println("Sensor auto-detect: no sensor responding (will retry)");
}

float cToF(float c) { return c * 9.0f / 5.0f + 32.0f; }

void setRgb(uint8_t r, uint8_t g, uint8_t b)
{
  ledcWrite(LED_RGB_R, r);
  ledcWrite(LED_RGB_G, g);
  ledcWrite(LED_RGB_B, b);
}

void getAverages(float &indoorTemp, float &indoorHum,
                 float &outdoorTemp, float &outdoorHum,
                 int &indoorCount, int &outdoorCount);
void updateStatusLeds();

enum RecState
{
  REC_WAITING,
  REC_OPEN,
  REC_CAUTION,
  REC_MARGINAL,
  REC_CLOSED
};
RecState computeRecommendation(float iT, float iH, float oT, float oH,
                               int iCnt, int oCnt);

// Dew point (°C) from temperature (°C) and relative humidity (%).
// Magnus formula; log() here is the natural log, which is what it wants.
float dewPointC(float tempC, float rhPct)
{
  const float a = 17.271f, b = 237.7f;
  float gamma = (a * tempC) / (b + tempC) + log(rhPct / 100.0f);
  return (b * gamma) / (a - gamma);
}

NodeReading *findOrCreate(const char *id)
{
  for (int i = 0; i < nodeCount; i++)
  {
    if (strcmp(nodes[i].id, id) == 0)
      return &nodes[i];
  }
  if (nodeCount >= MAX_NODES)
    return nullptr;
  NodeReading *n = &nodes[nodeCount++];
  strncpy(n->id, id, sizeof(n->id) - 1);
  n->id[sizeof(n->id) - 1] = '\0';
  n->valid = false;
  return n;
}

// Push one raw reading into the node's rolling window and refresh its
// smoothed (mean) values. Window length is READING_HISTORY_N (≈5 min).
void pushNodeReading(NodeReading *n, float tempC, float humidity)
{
  n->tempHist[n->histIdx] = tempC;
  n->humHist[n->histIdx] = humidity;
  n->histIdx = (n->histIdx + 1) % READING_HISTORY_N;
  if (n->histCount < READING_HISTORY_N)
    n->histCount++;

  float sumT = 0, sumH = 0;
  for (uint8_t i = 0; i < n->histCount; i++)
  {
    sumT += n->tempHist[i];
    sumH += n->humHist[i];
  }
  n->temp_c = sumT / n->histCount;
  n->humidity = sumH / n->histCount;
}

void getAverages(float &indoorTemp, float &indoorHum,
                 float &outdoorTemp, float &outdoorHum,
                 int &indoorCount, int &outdoorCount)
{
  indoorTemp = indoorHum = outdoorTemp = outdoorHum = 0;
  indoorCount = outdoorCount = 0;

  unsigned long now = millis();
  for (int i = 0; i < nodeCount; i++)
  {
    if (!nodes[i].valid)
      continue;
    if (now - nodes[i].last_seen_ms > FRESH_MS)
      continue;

    if (strcmp(nodes[i].location, "outdoor") == 0)
    {
      outdoorTemp += nodes[i].temp_c;
      outdoorHum += nodes[i].humidity;
      outdoorCount++;
    }
    else
    {
      indoorTemp += nodes[i].temp_c;
      indoorHum += nodes[i].humidity;
      indoorCount++;
    }
  }

  // No data for a side: report NaN (serialized as JSON null) instead of
  // a fabricated 0, so the dashboard shows "—" rather than 32 °F / 0 %.
  if (!indoorCount)
  {
    indoorTemp = NAN;
    indoorHum = NAN;
  }
  if (!outdoorCount)
  {
    outdoorTemp = NAN;
    outdoorHum = NAN;
  }

  if (indoorCount)
  {
    indoorTemp /= indoorCount;
    indoorHum /= indoorCount;
  }

  if (outdoorCount)
  {
    outdoorTemp /= outdoorCount;
    outdoorHum /= outdoorCount;
  }
}

void updateStatusLeds()
{
  float iT, iH, oT, oH;
  int iCnt, oCnt;
  getAverages(iT, iH, oT, oH, iCnt, oCnt);

  unsigned long now = millis();
  bool indoorFresh = false;
  bool outdoorFresh = false;
  for (int i = 0; i < nodeCount; i++)
  {
    if (!nodes[i].valid)
      continue;
    if (now - nodes[i].last_seen_ms > FRESH_MS)
      continue;
    if (strcmp(nodes[i].location, "outdoor") == 0)
      outdoorFresh = true;
    else
      indoorFresh = true;
  }
  digitalWrite(LED_INDOOR, indoorFresh ? HIGH : LOW);
  digitalWrite(LED_OUTDOOR, outdoorFresh ? HIGH : LOW);

  switch (computeRecommendation(iT, iH, oT, oH, iCnt, oCnt))
  {
  case REC_OPEN:
    setRgb(0, 180, 0);
    break; // green
  case REC_CAUTION:
    setRgb(220, 120, 0);
    break; // amber
  case REC_MARGINAL:
    setRgb(180, 100, 0);
    break; // dim orange
  case REC_CLOSED:
    setRgb(200, 0, 0);
    break; // red
  default:
    setRgb(0, 0, 0);
    break; // off = waiting for data
  }

  if (wifiBlinkUntil == 0)
  {
    // digitalWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
    ledcWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? 64 : 0);
  }
}

// Hysteresis state machine: latch OPEN once outdoor is cooler by
// TEMP_DELTA_OPEN_C, hold it until the margin shrinks below
// TEMP_DELTA_CLOSE_C (or outdoor humidity goes bad). Both the RGB LED
// and the dashboard text go through here so they can never disagree.
RecState computeRecommendation(float iT, float iH, float oT, float oH,
                               int iCnt, int oCnt)
{
  (void)iH; // indoor humidity is display-only;
  static bool openLatched = false;

  if (iCnt == 0 || oCnt == 0)
  {
    openLatched = false;
    return REC_WAITING;
  }

  // Moisture gate, v2: outdoor air must be drier (lower dew point) than indoor air.
  bool moistureOk;
  moistureOk = (dewPointC(oT, oH) <= dewPointC(iT, iH) - DEWPOINT_MARGIN_C);
  bool coolToOpen = (oT <= iT - TEMP_DELTA_OPEN_C);
  bool coolToHold = (oT <= iT - TEMP_DELTA_CLOSE_C);

  if (!openLatched)
  {
    if (coolToOpen && moistureOk)
      openLatched = true;
  }
  else if (!coolToHold || !moistureOk)
  {
    openLatched = false;
  }

  if (openLatched)
    return REC_OPEN;
  if (coolToOpen && !moistureOk)
    return REC_CAUTION;
  if (oT < iT)
    return REC_MARGINAL;
  return REC_CLOSED;
}

const char *recStateText(RecState s)
{
  switch (s)
  {
  case REC_OPEN:
    return "✅ OPEN windows / doors — outdoor air is cooler and drier";
  case REC_CAUTION:
    return "⚠️ Cooler outside but more humid — open only if you accept more moisture";
  case REC_MARGINAL:
    return "➖ Slightly cooler outside — marginal benefit";
  case REC_CLOSED:
    return "❌ Keep closed — outdoor is warmer or not beneficial";
  default:
    return "Waiting for indoor & outdoor data…";
  }
}

const char *getRecommendation(float iT, float iH, float oT, float oH,
                              int iCnt, int oCnt)
{
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
        .avg-table { width:100%; border-collapse:collapse; }
        .avg-table th, .avg-table td { padding:0.35rem 0.25rem; border-bottom:1px solid #334155; text-align:left; }
        .avg-table th { color:var(--accent); }
        .avg-table td:first-child { color:#94a3b8; }
        .avg-table td:not(:first-child) { font-weight:600; }
        .avg-table tr:last-child td { border-bottom:none; }
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
      <footer>
        Auto-refreshes every 15 s · Node values are 5-min rolling averages · ESP32 Climate Hub
      </footer>

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

        function fmtAge(sec) {
          if (sec < 60) return '< 1 min';
          const m = Math.floor(sec / 60);
          if (m < 60) return m + (m === 1 ? ' min' : ' mins');
          const h = Math.floor(sec / 3600);
          if (h < 24) return h + (h === 1 ? ' hr' : ' hrs');
          return '> 1 day';
        }

        function dewPoint(tc, rh) {
          if (tc == null || rh == null || isNaN(tc) || isNaN(rh) || rh <= 0) return null;
          const a = 17.271, b = 237.7;
          const gamma = (a * tc) / (b + tc) + Math.log(rh / 100);
          return (b * gamma) / (a - gamma);
        }

        function fmtDew(tc, rh) {
          const dp = dewPoint(tc, rh);
          return dp == null ? '—' : fmtTemp(dp, dp * 9 / 5 + 32);
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
            sum.innerHTML = `
              <h2>Averages</h2>
              <table class="avg-table">
                <tr><th>Averages</th><th>Indoor</th><th>Outdoor</th></tr>
                <tr><td>Temp</td><td>${d.indoor.temp_c == null ? '—' : fmtTemp(d.indoor.temp_c, d.indoor.temp_f)}</td><td>${d.outdoor.temp_c == null ? '—' : fmtTemp(d.outdoor.temp_c, d.outdoor.temp_f)}</td></tr>
                <tr><td>Humidity</td><td>${d.indoor.humidity == null ? '—' : d.indoor.humidity.toFixed(0) + ' %'}</td><td>${d.outdoor.humidity == null ? '—' : d.outdoor.humidity.toFixed(0) + ' %'}</td></tr>
                <tr><td>Dew Point</td><td>${fmtDew(d.indoor.temp_c, d.indoor.humidity)}</td><td>${fmtDew(d.outdoor.temp_c, d.outdoor.humidity)}</td></tr>
              </table>
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
                <div class="row"><span class="label">Dew Point</span><span class="val">${fmtDew(n.temp_c, n.humidity)}</span></div>
                <div class="row"><span class="label">RSSI</span><span class="val">${n.rssi === 0 ? 'local' : n.rssi + ' dBm'}</span></div>
                <div class="row"><span class="label">Last seen</span><span class="val">${fmtAge(age)}</span></div>
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

void handleRoot()
{
  wifiBlinkUntil = millis() + 120;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus()
{
  wifiBlinkUntil = millis() + 80;

  float iT, iH, oT, oH;
  int iCnt, oCnt;
  getAverages(iT, iH, oT, oH, iCnt, oCnt);

  JsonDocument doc;
  doc["recommendation"] = getRecommendation(iT, iH, oT, oH, iCnt, oCnt);

  JsonObject indoor = doc["indoor"].to<JsonObject>();
  indoor["temp_c"] = iT;
  indoor["temp_f"] = cToF(iT);
  indoor["humidity"] = iH; // null when indoor temp fell back to the hub TMP36
  indoor["count"] = iCnt;

  JsonObject outdoor = doc["outdoor"].to<JsonObject>();
  outdoor["temp_c"] = oT;
  outdoor["temp_f"] = cToF(oT);
  outdoor["humidity"] = oH;
  outdoor["count"] = oCnt;

  JsonArray arr = doc["nodes"].to<JsonArray>();
  unsigned long now = millis();
  for (int i = 0; i < nodeCount; i++)
  {
    if (!nodes[i].valid)
      continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = nodes[i].id;
    o["location"] = nodes[i].location;
    o["temp_c"] = nodes[i].temp_c;
    o["temp_f"] = cToF(nodes[i].temp_c);
    o["humidity"] = nodes[i].humidity;
    o["rssi"] = nodes[i].rssi;
    o["age_s"] = (now - nodes[i].last_seen_ms) / 1000UL;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleReading()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "application/json", "{\"error\":\"POST only\"}");
    return;
  }

  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  const char *id = doc["id"] | "unknown";
  NodeReading *n = findOrCreate(id);
  if (!n)
  {
    server.send(503, "application/json", "{\"error\":\"too many nodes\"}");
    return;
  }

  strncpy(n->location, doc["location"] | "indoor", sizeof(n->location) - 1);
  n->location[sizeof(n->location) - 1] = '\0';
  pushNodeReading(n, doc["temp_c"] | 0.0f, doc["humidity"] | 0.0f);
  n->rssi = doc["rssi"] | 0;
  n->last_seen_ms = millis();
  n->valid = true;

  rxBlinkUntil = millis() + 150;
  Serial.printf("RX %s (%s): %.1f°C  %.0f%%  (smoothed %.1f°C / %d samples)\n",
                n->id, n->location, (double)(doc["temp_c"] | 0.0f),
                (double)(doc["humidity"] | 0.0f), n->temp_c, n->histCount);

  server.send(200, "application/json", "{\"status\":\"ok\"}");
  updateStatusLeds();
}

void handleNotFound()
{
  server.send(404, "text/plain", "Not found");
}

void connectWiFi()
{
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.print("WiFi OK  IP: ");
    Serial.println(WiFi.localIP());
    Serial.println(">>> Set SERVER_HOST in sensor secrets.h to the IP above <<<");
    // digitalWrite(LED_WIFI, HIGH);
    ledcWrite(LED_WIFI, 64); // half-bright to indicate Wi-Fi is up
  }
  else
  {
    Serial.println("\nWiFi failed – starting soft AP for setup");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID_FALLBACK, AP_PASS_FALLBACK);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    // digitalWrite(LED_WIFI, LOW);
    ledcWrite(LED_WIFI, 0); // off to indicate Wi-Fi is down
  }
}

void setupLeds()
{
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_RX, OUTPUT);
  pinMode(LED_INDOOR, OUTPUT);
  pinMode(LED_OUTDOOR, OUTPUT);

  // RGB LED for open indicator
  ledcAttach(LED_RGB_R, RGB_FREQ, RGB_RES);
  ledcAttach(LED_RGB_G, RGB_FREQ, RGB_RES);
  ledcAttach(LED_RGB_B, RGB_FREQ, RGB_RES);

  // PWM setup for Wi-Fi LED
  ledcAttach(LED_WIFI, RGB_FREQ, RGB_RES);
  ledcWrite(LED_WIFI, 0);
  // digitalWrite(LED_WIFI, LOW);
  digitalWrite(LED_RX, LOW);
  digitalWrite(LED_INDOOR, LOW);
  digitalWrite(LED_OUTDOOR, LOW);
  setRgb(0, 0, 0);
}

void testRgbCycle()
{
  int delayTime = 500;
  int pwmDelay = 10;
  int offDelay = 300;
  int repeat = 1;
  int j = 0;

  Serial.println("RGB test: RED, GREEN, BLUE, YELLOW, WHITE, OFF");
  for (int n = 0; n < repeat; n++)
  {
    Serial.printf("Test %d\n", n + 1);
    setRgb(255, 0, 0);
    delay(delayTime);
    setRgb(0, 255, 0);
    delay(delayTime);
    setRgb(0, 0, 255);
    delay(delayTime);
    setRgb(255, 160, 0);
    delay(delayTime);
    setRgb(200, 200, 200);
    delay(delayTime);
    setRgb(0, 0, 0);
    delay(offDelay);
  }

  Serial.println("RGB test: fade green");
  for (int i = 0; i <= 180; i += 5)
  {
    setRgb(0, i, 0);
    delay(pwmDelay);
  }
  for (int i = 180; i >= 0; i -= 5)
  {
    setRgb(0, i, 0);
    delay(pwmDelay);
  }

  Serial.println("RGB test: fade yellow/amber");
  for (int i = 0; i <= 200; i += 5)
  {
    j = (i * 120) / 200;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }
  for (int i = 200; i >= 0; i -= 5)
  {
    j = (i * 120) / 200;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }

  setRgb(0, 0, 0);
  delay(offDelay);

  Serial.println("RGB test: fade softer yellow");
  for (int i = 0; i <= 180; i += 5)
  {
    j = (i * 100) / 180;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }
  for (int i = 180; i >= 0; i -= 5)
  {
    j = (i * 100) / 180;
    setRgb(i, j, 0);
    delay(pwmDelay);
  }

  Serial.println("RGB test: fade red");
  for (int i = 0; i <= 255; i += 5)
  {
    setRgb(i, 0, 0);
    delay(pwmDelay);
  }
  for (int i = 255; i >= 0; i -= 5)
  {
    setRgb(i, 0, 0);
    delay(pwmDelay);
  }

  Serial.println("RGB test: fade white");
  for (int i = 0; i <= 255; i += 5)
  {
    setRgb(i, i, i);
    delay(pwmDelay);
  }
  for (int i = 255; i >= 0; i -= 5)
  {
    setRgb(i, i, i);
    delay(pwmDelay);
  }

  Serial.println("RGB test: OFF");
  setRgb(0, 0, 0);
  delay(offDelay);
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Climate Web Server ===");

  setupLeds();
  testRgbCycle();

  memset(nodes, 0, sizeof(nodes));
  connectWiFi();

  detectSensor(); // auto-detect DHT11 vs DHT22

  NodeReading *hub = findOrCreate("hub");
  strncpy(hub->location, "indoor", sizeof(hub->location) - 1);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/reading", HTTP_POST, handleReading);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
  updateStatusLeds();
}

void loop()
{
  server.handleClient();

  unsigned long now = millis();

  digitalWrite(LED_RX, (now < rxBlinkUntil) ? HIGH : LOW);

  if (now < wifiBlinkUntil)
  {
    // digitalWrite(LED_WIFI, LOW);
    ledcWrite(LED_WIFI, 255); // full-bright on browser activity
  }
  else if (wifiBlinkUntil != 0 && now >= wifiBlinkUntil)
  {
    wifiBlinkUntil = 0;
    // digitalWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
    ledcWrite(LED_WIFI, WiFi.status() == WL_CONNECTED ? 64 : 0);
  }

  static unsigned long lastHubRead = 0;
  if (now - lastHubRead > SENSOR_INTERVAL_MS)
  {
    lastHubRead = now;
    if (!dht)
    {
      detectSensor();
    }
    if (dht)
    {
      float h = dht->readHumidity();
      float t = dht->readTemperature();
      if (isnan(h) || isnan(t))
      {
        Serial.println("Hub DHT read failed");
        if (++hubReadFailures >= 3)
        {
          dht = nullptr;
          hubReadFailures = 0;
        }
      }
      else if (t < 1.0 && h < 1.0)
      {
        Serial.println("Hub DHT still warming up – skipping");
      }
      else
      {
        hubReadFailures = 0;
        NodeReading *hub = findOrCreate("hub");
        pushNodeReading(hub, t, h);
        hub->rssi = 0; // local sensor, no radio
        hub->last_seen_ms = now;
        hub->valid = true;
        Serial.printf("Hub DHT: %.1f °C / %.1f %%\n", t, h);
      }
    }
  }

  static unsigned long lastLedUpdate = 0;
  if (now - lastLedUpdate > 2000)
  {
    lastLedUpdate = now;
    updateStatusLeds();
  }
}
