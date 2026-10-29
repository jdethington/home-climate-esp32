#pragma once

// Copy this file to secrets.h in both sensor-node/src/ and web-server/src/
// Then fill in your real values. secrets.h is gitignored.

// ---- Wi-Fi ----
#define WIFI_SSID     "YourNetworkName"
#define WIFI_PASSWORD "YourPassword"

// ---- Server (for sensor nodes) ----
// Set this to the IP address the web-server ESP32 gets on your LAN.
// You can find it in the Serial monitor of the web-server board after it boots.
#define SERVER_HOST   "192.168.1.50"   // change after first boot of server
#define SERVER_PORT   80

// ---- Optional: static IP for the web-server board (recommended) ----
// Uncomment and set these in web-server only if you want a fixed address.
// #define USE_STATIC_IP
// #define STATIC_IP     192,168,1,50
// #define GATEWAY       192,168,1,1
// #define SUBNET        255,255,255,0
// #define DNS1          8,8,8,8
