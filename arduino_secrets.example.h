// arduino_secrets.example.h — TEMPLATE.
//
// Copy this file to `arduino_secrets.h` and fill in your own values:
//     cp arduino_secrets.example.h arduino_secrets.h
//
// arduino_secrets.h is git-ignored, so your real credentials never get committed.
#pragma once

#define SECRET_WIFI_SSID     "YOUR_WIFI_SSID"
#define SECRET_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define SECRET_VICTRON_IP    "192.168.1.100"   // your Victron GX IP address
#define SECRET_VRM_ID        "xxxxxxxxxxxx"    // VRM Portal ID (Settings -> VRM online portal -> VRM Portal ID)

#define SECRET_HOMEKIT_CODE  "83719264"        // 8 digits, no dashes; typed in Home as XXX-XX-XXX (pick your own)
#define SECRET_DEVICE_SERIAL "HQ0000AB0CD"     // your MultiPlus serial number (cosmetic)
