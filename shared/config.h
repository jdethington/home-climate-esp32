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
#define TEMP_DELTA_C         2.0f      // outdoor must be at least this many °C cooler
#define MAX_OUTDOOR_RH       65.0f     // don't recommend if outdoor RH above this %

// HTTP endpoint the sensors POST to
#define API_PATH             "/api/reading"

// Soft AP fallback name if Wi-Fi fails (server only)
#define AP_SSID_FALLBACK     "ClimateHub-Setup"
#define AP_PASS_FALLBACK     "climate123"
