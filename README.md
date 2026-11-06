# Home Climate Monitor (ESP32)

Compare indoor and outdoor temperature & humidity, then get a clear recommendation on whether to open windows or doors for free cooling.

Designed for a Phoenix / desert climate home, but the thresholds are fully tunable.

---

## What it does

- **Sensor nodes** (ESP32 + DHT11/DHT22) read temperature and humidity and POST JSON to a central hub over your home Wi‑Fi.
- **Web-server hub** (one ESP32) receives those readings, serves a live dashboard, drives status LEDs, and reads its own DHT11/DHT22 — the hub itself is an indoor node.
- The dashboard shows an averages table (Temp / Humidity / Dew Point × Indoor / Outdoor), per-node detail with a relative "Last seen", a recommendation banner, and a **°C / °F toggle**. Missing data shows "—" instead of a fabricated zero.

You can start with the **hub + 1 outdoor node** — the hub counts as an indoor node — and add more nodes later (up to 8 tracked).

---

## Hardware

| Role             | Board           | Sensors / extras                                   |
| ---------------- | --------------- | -------------------------------------------------- |
| Hub / web server | 1× ESP32 DevKit | DHT11/22 (as an indoor node), status LEDs, RGB LED |
| Indoor nodes     | 1–2× ESP32      | DHT22 each                                         |
| Outdoor nodes    | 1–2× ESP32      | DHT11 each, weatherproof enclosure                 |

**Purchased / typical parts**

- ESP32 development boards
- DHT11 temperature + humidity modules
- DHT11/22 temperature + humidity module (on the hub, same wiring as the nodes)
- Discrete LEDs + 220–330 Ω resistors
- Common-cathode RGB LED (4 pins)
- Breadboard / jumpers for prototyping

**Planned later:** pressure-capable sensors (e.g. BME280 / SHT4x) in place of DHT11.

---

## Architecture

```
┌─────────────┐   ┌─────────────┐
│ Outdoor 1   │   │ Outdoor 2   │   (optional)
│ ESP32+DHT11 │   │ ESP32+DHT11 │
└──────┬──────┘   └──────┬──────┘
       │  HTTP POST      │
       └────────┬────────┘
                │
         ┌──────▼──────┐
         │  Web Server │  ← http://<hub-ip>/
         │ ESP32 hub   │  + DHT + LEDs + RGB
         └──────▲──────┘
                │
       ┌────────┴────────┐
       │  HTTP POST      │
┌──────┴──────┐   ┌──────┴──────┐
│ Indoor 1    │   │ Indoor 2    │   (optional)
│ ESP32+DHT11 │   │ ESP32+DHT11 │
└─────────────┘   └─────────────┘
```

- Sensors POST to `http://<SERVER_HOST>/api/reading` about every 30 seconds.
- Hub serves `/` (dashboard) and `/api/status` (JSON).
- No cloud, no MQTT required — all local Wi‑Fi.

---

## Prerequisites

- [VS Code](https://code.visualstudio.com/)
- [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) extension
- Git (optional, for versioning)
- Home Wi‑Fi name and password

---

## Quick start

### 1. Open the project

```bash
cd home-climate-esp32
code .    # or open the .code-workspace file
```

### 2. Create secrets files

```bash
cp shared/secrets.example.h sensor-node/src/secrets.h
cp shared/secrets.example.h web-server/src/secrets.h
```

Edit **both** `secrets.h` files:

```cpp
#define WIFI_SSID     "YourNetworkName"
#define WIFI_PASSWORD "YourPassword"
#define SERVER_HOST   "192.168.0.50"   // set after hub boots — see step 3
#define SERVER_PORT   80
```

`secrets.h` is gitignored — never commit real passwords.

### 3. Flash the web-server hub first

1. Connect the hub ESP32 by USB.
2. Open the `web-server` project in PlatformIO.
3. Upload: `pio run -t upload` (or the Upload button).
4. Open **Serial Monitor at 115200 baud**.
5. Note the IP, e.g. `WiFi OK  IP: 192.168.0.136`.
6. Put that IP into **both** `secrets.h` files as `SERVER_HOST`.
7. Re-upload the hub if you changed anything else; re-upload **sensors** after setting `SERVER_HOST`.

On boot the hub runs an **RGB color test**, then joins Wi‑Fi and starts the HTTP server.

### 4. Flash sensor nodes

From the `sensor-node` folder, use the matching environment so you never edit `main.cpp` for identity:

| Board     | Environment / command           |
| --------- | ------------------------------- |
| Indoor 1  | `pio run -e indoor1 -t upload`  |
| Indoor 2  | `pio run -e indoor2 -t upload`  |
| Outdoor 1 | `pio run -e outdoor1 -t upload` |
| Outdoor 2 | `pio run -e outdoor2 -t upload` |

Serial Monitor should show Wi‑Fi OK, DHT readings, and `POST ... → HTTP 200 (OK)`.

### 5. Open the dashboard

On any device on the same Wi‑Fi:

```
http://<hub-ip>/
```

You should see the recommendation, the indoor/outdoor averages table, and a card for each node — including the hub itself as an indoor node. Use the **°C / °F** switch at the top; the choice is remembered in the browser.

---

## Wiring

### DHT11 / DHT22 → sensor ESP32

| DHT11 / DHT22 | ESP32      |
| ------------- | ---------- |
| VCC           | 3.3V       |
| GND           | GND        |
| DATA          | **GPIO 4** |

Most modules already have a pull-up. Pin is set in `shared/config.h` as `DHT_PIN`.
Wiring is identical for both sensor models. The firmware auto-detects which one
is connected at boot (tries DHT22 first, falls back to DHT11), so any board can
take either sensor with no code or build changes — just flash the right
`indoor`/`outdoor` environment for its location.

### DHT11 / DHT22 → hub ESP32

Same wiring as a sensor node — the hub's own sensor is on **GPIO 4**, and the firmware auto-detects DHT11 vs DHT22 at boot:

| DHT11 / DHT22 | ESP32      |
| ------------- | ---------- |
| VCC           | 3.3V       |
| GND           | GND        |
| DATA          | **GPIO 4** |

The hub reads its sensor every 30 seconds and appears on the dashboard as the `hub` indoor node, with its own rolling average like any other node. If its sensor fails, the hub card goes stale after ~3 minutes like any dead node — there is no temperature-only fallback anymore.

### Hub status LEDs (as used in current firmware)

| Function                                            | GPIO   | Notes                                                             |
| --------------------------------------------------- | ------ | ----------------------------------------------------------------- |
| Sensor POST received (blink)                        | **2**  | Onboard LED                                                       |
| Wi‑Fi connected (+ brief blink on browser activity) | **16** | Half-bright (PWM 64) when joined; full bright on browser activity |
| RGB Red                                             | **13** | PWM                                                               |
| RGB Green                                           | **14** | PWM                                                               |
| RGB Blue                                            | **18** | PWM                                                               |
| Indoor data fresh (&lt; 3 min)                      | **19** |                                                                   |
| Outdoor data fresh (&lt; 3 min)                     | **21** |                                                                   |

Pin choices avoid the ESP32 boot strapping pins (0, 5, 12, 15). GPIO 2 is kept
because it drives the onboard LED (active-HIGH on this board — verified: the
LED lights when GPIO 2 is HIGH). Note the outdoor LED currently uses GPIO 21,
which is the board's default I2C SDA pin — it will need to move (GPIO 25 was
used before) before adding an I2C sensor. The DHT data pin (GPIO 4) is
digital-only use, so its ADC2 membership doesn't matter.

**RGB (common-cathode, 4 wires)**

```
GPIO 13 ──[220–330Ω]── Red
GPIO 14 ──[220–330Ω]── Green
GPIO 18 ──[220–330Ω]── Blue
GND ─────────────────── Cathode (usually the longest pin)
```

**RGB meaning**

| Color          | Meaning                                 |
| -------------- | --------------------------------------- |
| Green          | Open windows / doors                    |
| Red            | Keep closed                             |
| Yellow / amber | Marginal or outdoor humidity high       |
| Off            | Waiting for indoor **and** outdoor data |

Discrete LEDs: GPIO → resistor → LED anode; cathode → GND.

---

## Recommendation logic

Configured in `shared/config.h`:

```cpp
#define TEMP_DELTA_OPEN_C   2.0f   // latch OPEN when outdoor is this many °C cooler
#define TEMP_DELTA_CLOSE_C  1.0f   // drop OPEN when the margin shrinks below this
#define DEWPOINT_MARGIN_C   1.0f   // outdoor dew point must be this many °C below indoor
#define READING_HISTORY_N   10     // per-node readings averaged (≈5 min at 30 s)
```

- **Smoothing** — the hub keeps each node's last 10 readings (~5 minutes) and
  decides on the rolling average, so one noisy DHT11 read can't flip the
  recommendation. Node cards on the dashboard show these smoothed values.
- **Hysteresis** — the OPEN recommendation latches on once outdoor is ≥ 2 °C
  cooler (and the dew-point gate passes) and stays on until the margin shrinks
  below 1 °C or the dew-point gate fails. No flapping on borderline evenings.
- **Moisture gate** — outdoor air must be drier than indoor air: outdoor dew
  point ≤ indoor dew point − `DEWPOINT_MARGIN_C`. (The old outdoor-RH cap is
  gone; `MAX_OUTDOOR_RH` is no longer used.)
- **OPEN** — outdoor cooler by ≥ `TEMP_DELTA_OPEN_C` and the dew-point gate passes
- **Caution** — cooler outside but more humid (higher dew point)
- **Marginal** — slightly cooler, small benefit
- **Keep closed** — outdoor warmer or not beneficial
- **Hub as indoor node** — the hub's own DHT counts as an indoor node, so the
  recommendation works with zero remote indoor nodes. If the hub sensor fails,
  its card goes stale like any dead node.

The RGB LED and the dashboard banner share one decision function, so they can
never disagree. Rebuild and re-upload the **web-server** after changing thresholds.

---

## Project layout

```
home-climate-esp32/
├── sensor-node/           # PlatformIO: DHT11 nodes (multi-env)
│   ├── platformio.ini     # indoor1/2, outdoor1/2
│   └── src/main.cpp
├── web-server/            # PlatformIO: hub + dashboard + LEDs + DHT
│   ├── platformio.ini
│   └── src/main.cpp
├── shared/
│   ├── config.h           # interval, pins, thresholds
│   └── secrets.example.h  # template only
├── docs/
│   └── outdoor-setup.md   # Phoenix enclosure & power notes
├── .gitignore
├── home-climate-esp32.code-workspace
└── README.md
```

---

## Serial Monitor

Always **115200** baud (set in each `platformio.ini` as `monitor_speed`).

Useful hub lines:

```
WiFi OK  IP: 192.168.0.x
HTTP server started
Sensor auto-detect: DHT22
Hub DHT: 25.1 °C / 32.0 %
RX in1 (indoor): 25.5°C  32%
```

Useful sensor lines:

```
WiFi OK  IP: ...
Temp: 25.5 °C  Humidity: 32.0 %  RSSI: -66
POST http://192.168.0.x:80/api/reading → HTTP 200  (OK)
```

---

## Troubleshooting

| Symptom                            | What to try                                                                                                                                                   |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Hub starts soft AP (`192.168.4.1`) | Wrong SSID/password in **web-server** `secrets.h`; re-upload                                                                                                  |
| Sensor `HTTP -1` / `-11`           | Confirm hub IP in sensor `secrets.h`, re-upload sensor; same 2.4 GHz Wi‑Fi; retries are built in                                                              |
| DHT11/DHT22 NaN or 0 °C / 0 %      | Wiring, 3.3V power, wait a few seconds after boot                                                                                                             |
| Hub DHT NaN / hub card stale       | Check DHT wiring on GPIO 4 and 3.3V power; the hub re-detects the sensor after 3 failed reads                                                                 |
| Upload fails                       | Hold **BOOT** on some boards during upload; try another USB cable/port; if you see packet/checksum errors, lower `upload_speed` to 460800 in `platformio.ini` |
| Dashboard “Waiting…”               | Need at least one **indoor** (the hub itself counts) and one **outdoor** node reporting within ~3 minutes                                                     |
| Garbage on Serial                  | Set baud rate to **115200**                                                                                                                                   |
| RGB colors swapped                 | Swap the R/G/B wires on GPIO 13/14/18                                                                                                                         |

---

## Later upgrades

- Replace DHT11 with **BME280 / BME680 / SHT4x** (pressure + better accuracy)
- Weatherproof outdoor nodes (see `docs/outdoor-setup.md`)
- Static IP for the hub (optional defines in `secrets.example.h`)
- MQTT / Home Assistant if you want more integration
- History logging (LittleFS or external DB)
- Notifications when conditions become favorable

---

## License

MIT — use and modify freely.
