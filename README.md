# ESP32-C6 Secure Temperature Telemetry

[English](README.md) | [Русский](README_RU.md)

`ESP32-C6` firmware project (`PlatformIO` + `Arduino`) for:

- reading temperature from `MAX31875` over `I2C` (`SDA=GPIO2`, `SCL=GPIO3`)
- encrypting telemetry with `AES-256-CBC`
- sending encrypted payload to HTTP server over Wi-Fi
- verbose diagnostics in serial monitor (Wi-Fi, I2C scan, temperature, encryption)
- storing AES key in persistent `NVS` (generated on first boot)

## Features

- **Sensor**: `MAX31875R0TZS+T`, I2C address configurable in `include/secrets.h`
- **Crypto**: `AES-256-CBC` + `PKCS7` padding
- **Payload format**: `base64(IV[16] + ciphertext)`
- **Key management**:
  - first boot: random AES-256 key is generated and stored in NVS
  - next boots: same key is loaded from NVS
  - after flash erase: new key is generated automatically
- **Logging**:
  - boot info
  - Wi-Fi status + network details (IP, gateway, RSSI, MAC)
  - I2C scan results
  - measured temperature
  - encrypted payload (`payload_b64`)
- **Utility script**: interactive `flash_test_com49.bat` menu for build/flash/monitor

## Project Structure

- `src/main.cpp` - main firmware logic
- `include/secrets.example.h` - template with user configuration
- `include/secrets.h` - local private config (ignored by git)
- `flash_test_com49.bat` - helper menu script (build/flash/monitor)
- `platformio.ini` - PlatformIO environment settings

## Setup

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Configure:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `SERVER_URL`
   - `MEASURE_INTERVAL_MS`
   - `SEND_INTERVAL_MS`
   - `MAX31875_I2C_ADDR` (default `0x48`)

## Build and Flash

### Option A: PlatformIO commands

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

### Option B: helper menu script (recommended on Windows)

```powershell
.\flash_test_com49.bat
```

Menu options:

- `0` - Build project
- `1` - Flash device (asks whether to erase flash first)
- `2` - Open serial monitor
- `3` - Exit

## Runtime Behavior

On startup firmware:

1. initializes I2C
2. scans `0x48..0x4F` for MAX31875
3. connects to Wi-Fi and prints network details
4. loads AES key from NVS or generates new key
5. prints AES key in `HEX` and `B64` (for test/decrypt workflows)

Then in loop:

- every `MEASURE_INTERVAL_MS`: read sensor, log temperature, encrypt payload
- every `SEND_INTERVAL_MS`: send last encrypted payload to server

## Server Payload Format

```json
{
  "device": "esp32c6",
  "enc": "aes-256-cbc",
  "payload_b64": "<base64(IV + ciphertext)>"
}
```

Plaintext before encryption:

```json
{
  "temperature_c": 34.0000,
  "ts_ms": 8235
}
```

## Browser Decryption Example (WebCrypto)

```html
<script>
async function decryptAesCbc(payloadB64, keyB64) {
  const payload = Uint8Array.from(atob(payloadB64), c => c.charCodeAt(0));
  const iv = payload.slice(0, 16);
  const ciphertext = payload.slice(16);
  const rawKey = Uint8Array.from(atob(keyB64), c => c.charCodeAt(0));

  const cryptoKey = await crypto.subtle.importKey(
    "raw",
    rawKey,
    { name: "AES-CBC" },
    false,
    ["decrypt"]
  );

  const plainBuffer = await crypto.subtle.decrypt(
    { name: "AES-CBC", iv },
    cryptoKey,
    ciphertext
  );

  return new TextDecoder().decode(plainBuffer);
}
</script>
```

## Security Notes

- Key logging is convenient for debug/demo, but unsafe for production.
- For production:
  - disable key output in logs
  - use `HTTPS` endpoints
  - consider ESP32 flash encryption / secure boot
  - rotate and protect keys operationally
