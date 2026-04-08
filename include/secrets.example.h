#ifndef SECRETS_H
#define SECRETS_H

// Wi-Fi credentials
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Server endpoint that accepts POST with JSON body.
// Example: http://192.168.1.100:3000/telemetry
#define SERVER_URL "http://YOUR_SERVER/telemetry"

// Timing configuration (milliseconds).
#define MEASURE_INTERVAL_MS 2000
#define SEND_INTERVAL_MS 5000

// MAX31875 I2C address range is 0x48..0x4F.
// Change if your ADDR pins are wired differently.
#define MAX31875_I2C_ADDR 0x48

// AES-256 key is generated automatically on first boot
// and stored in NVS (namespace "crypto", key "aes256key").
// If flash/NVS is erased, a new key is generated.

#endif
