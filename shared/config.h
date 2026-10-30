#pragma once

// Common configuration for all nodes

// How often sensor nodes read & send (milliseconds)
#define SENSOR_INTERVAL_MS   30000UL   // 30 seconds

// DHT11 data pin (change if you wire differently)
#define DHT_PIN              4
#define DHT_TYPE             DHT11

// Maximum number of sensor nodes the server tracks
#define MAX_NODES            8

// Recommendation thresholds (tune for Phoenix comfort)
// Hysteresis: latch OPEN when outdoor is cooler by TEMP_DELTA_OPEN_C,
// hold it until the margin shrinks below TEMP_DELTA_CLOSE_C.
#define TEMP_DELTA_OPEN_C    2.0f      // °C cooler to trigger the OPEN recommendation
#define TEMP_DELTA_CLOSE_C   1.0f      // °C cooler to keep holding it
#define MAX_OUTDOOR_RH       65.0f     // don't recommend if outdoor RH above this %

// Smoothing: the hub averages each node's last N readings before deciding.
// N = 10 at the default 30 s interval ≈ 5 minutes of readings.
#define READING_HISTORY_N    10

// HTTP endpoint the sensors POST to
#define API_PATH             "/api/reading"

// Soft AP fallback name if Wi-Fi fails (server only)
#define AP_SSID_FALLBACK     "ClimateHub-Setup"
#define AP_PASS_FALLBACK     "climate123"
