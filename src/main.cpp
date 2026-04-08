#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_log.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

#include "secrets.h"

namespace {
// -----------------------
// Конфигурация приложения
// -----------------------
// Единый тег для всех сообщений приложения в монитор порта.
constexpr const char *APP_TAG = "APP";
// Для AES-256 нужен ключ длиной 32 байта.
constexpr size_t AES_KEY_LEN = 32;
// Пространство имен/ключ в NVS для постоянного хранения AES-ключа.
constexpr const char *NVS_NS_CRYPTO = "crypto";
constexpr const char *NVS_AES_KEY = "aes256key";

constexpr uint8_t I2C_SDA_PIN = 2;
constexpr uint8_t I2C_SCL_PIN = 3;
#ifndef MAX31875_I2C_ADDR
constexpr uint8_t MAX31875_ADDR = 0x48;
#else
constexpr uint8_t MAX31875_ADDR = MAX31875_I2C_ADDR;
#endif
constexpr uint8_t TEMP_REGISTER = 0x00;

// -----------------------
// Состояние во время работы
// -----------------------
float lastMeasuredTempC = 0.0f;
bool hasTemperatureSample = false;
bool hasEncryptedPayload = false;
uint32_t lastMeasureMs = 0;
uint32_t lastSendMs = 0;
uint32_t measureCount = 0;
String lastCipherB64;
String lastRequestBody;
uint8_t runtimeAesKey[AES_KEY_LEN] = {0};
Preferences keyStore;

// -----------------------
// Вспомогательные функции логирования
// -----------------------
// Сознательно дублируем лог в три канала:
// 1) Serial.println
// 2) printf
// 3) ESP_LOG
// Это повышает шанс увидеть сообщения при разных терминалах/драйверах.
void logInfo(const String &msg) {
  Serial.println(msg);
  printf("[APP][I] %s\n", msg.c_str());
  ESP_LOGI(APP_TAG, "%s", msg.c_str());
}

void logWarn(const String &msg) {
  Serial.println(msg);
  printf("[APP][W] %s\n", msg.c_str());
  ESP_LOGW(APP_TAG, "%s", msg.c_str());
}

void logError(const String &msg) {
  Serial.println(msg);
  printf("[APP][E] %s\n", msg.c_str());
  ESP_LOGE(APP_TAG, "%s", msg.c_str());
}

const char *wifiStatusToText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

// Выводит полные сетевые параметры после подключения к Wi-Fi.
void printWifiDetails() {
  logInfo("Wi-Fi details:");
  logInfo("  SSID: " + WiFi.SSID());
  logInfo("  IP: " + WiFi.localIP().toString());
  logInfo("  Gateway: " + WiFi.gatewayIP().toString());
  logInfo("  Subnet: " + WiFi.subnetMask().toString());
  logInfo("  DNS: " + WiFi.dnsIP().toString());
  logInfo("  BSSID: " + WiFi.BSSIDstr());
  logInfo("  RSSI: " + String(WiFi.RSSI()) + " dBm");
  logInfo("  MAC: " + WiFi.macAddress());
}

// Преобразует массив байт в HEX-строку (верхний регистр) для логов.
String bytesToHex(const uint8_t *data, size_t len) {
  String out;
  out.reserve(len * 2);
  static const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < len; ++i) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

// Вспомогательная Base64-функция для вывода ключа и шифротекста.
String base64Encode(const uint8_t *data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  String out;
  out.reserve(outLen + 1);
  uint8_t *buffer = static_cast<uint8_t *>(malloc(outLen + 1));
  if (!buffer) {
    return "";
  }
  if (mbedtls_base64_encode(buffer, outLen + 1, &outLen, data, len) != 0) {
    free(buffer);
    return "";
  }
  buffer[outLen] = '\0';
  out = reinterpret_cast<char *>(buffer);
  free(buffer);
  return out;
}

// Читает температурный регистр MAX31875 по I2C и конвертирует в градусы C.
// При ошибке связи возвращает false и понятный текст причины.
bool readTemperatureC(float &tempC, String &errorText) {
  Wire.beginTransmission(MAX31875_ADDR);
  Wire.write(TEMP_REGISTER);
  const uint8_t txCode = Wire.endTransmission(false);
  if (txCode != 0) {
    errorText = "I2C write failed, code=" + String(txCode);
    return false;
  }

  const int bytesRead = Wire.requestFrom(static_cast<int>(MAX31875_ADDR), 2);
  if (bytesRead != 2) {
    errorText = "I2C read failed, bytes=" + String(bytesRead);
    return false;
  }

  int16_t raw = (static_cast<int16_t>(Wire.read()) << 8) | Wire.read();

  // Значение температуры MAX31875: 12-бит, дополнительный код, 0.0625 C/LSB.
  raw >>= 4;
  if (raw & 0x0800) {
    raw |= 0xF000;
  }

  tempC = static_cast<float>(raw) * 0.0625f;
  errorText = "";
  return true;
}

// Сканирует типичный диапазон адресов MAX31875, чтобы диагностировать проводку/адрес.
void scanI2CBusForTempSensor() {
  logInfo("I2C scan 0x48..0x4F:");
  bool foundAny = false;
  for (uint8_t addr = 0x48; addr <= 0x4F; ++addr) {
    Wire.beginTransmission(addr);
    const uint8_t code = Wire.endTransmission();
    if (code == 0) {
      Serial.print("  Found device at 0x");
      if (addr < 16) {
        Serial.print("0");
      }
      Serial.println(addr, HEX);
      char buf[48];
      snprintf(buf, sizeof(buf), "Found device at 0x%02X", addr);
      ESP_LOGI(APP_TAG, "%s", buf);
      foundAny = true;
    }
  }
  if (!foundAny) {
    logWarn("No devices found in MAX31875 address range.");
  }
}

// Шифрует plaintext алгоритмом AES-256-CBC + PKCS7.
// Формат результата: payload_b64 = base64( IV[16] + ciphertext ).
bool encryptAesCbcPkcs7(const uint8_t *key,
                        const uint8_t *plain,
                        size_t plainLen,
                        String &cipherB64) {
  const size_t paddedLen = ((plainLen / 16) + 1) * 16;
  uint8_t *padded = static_cast<uint8_t *>(malloc(paddedLen));
  uint8_t *cipher = static_cast<uint8_t *>(malloc(paddedLen));
  if (!padded || !cipher) {
    free(padded);
    free(cipher);
    return false;
  }

  memcpy(padded, plain, plainLen);
  const uint8_t padVal = static_cast<uint8_t>(paddedLen - plainLen);
  memset(padded + plainLen, padVal, padVal);

  uint8_t iv[16];
  for (size_t i = 0; i < sizeof(iv); i += 4) {
    uint32_t r = esp_random();
    iv[i + 0] = static_cast<uint8_t>(r & 0xFF);
    iv[i + 1] = static_cast<uint8_t>((r >> 8) & 0xFF);
    iv[i + 2] = static_cast<uint8_t>((r >> 16) & 0xFF);
    iv[i + 3] = static_cast<uint8_t>((r >> 24) & 0xFF);
  }

  uint8_t ivWork[16];
  memcpy(ivWork, iv, sizeof(iv));

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int rc = mbedtls_aes_setkey_enc(&aes, key, 256);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLen, ivWork, padded, cipher);
  }
  mbedtls_aes_free(&aes);

  if (rc != 0) {
    free(padded);
    free(cipher);
    return false;
  }

  const size_t payloadLen = sizeof(iv) + paddedLen;
  uint8_t *payload = static_cast<uint8_t *>(malloc(payloadLen));
  if (!payload) {
    free(padded);
    free(cipher);
    return false;
  }
  memcpy(payload, iv, sizeof(iv));
  memcpy(payload + sizeof(iv), cipher, paddedLen);

  cipherB64 = base64Encode(payload, payloadLen);
  free(payload);
  free(padded);
  free(cipher);
  return !cipherB64.isEmpty();
}

// Загружает/создает постоянный AES-256 ключ в NVS.
// Первый запуск (или после erase flash): генерируется случайный ключ и сохраняется.
// Следующие запуски: загружается тот же ключ, чтобы старые данные можно было расшифровать.
bool loadOrCreateRuntimeAesKey() {
  if (!keyStore.begin(NVS_NS_CRYPTO, false)) {
    logError("NVS open failed for AES key storage.");
    return false;
  }

  const size_t loadedLen = keyStore.getBytes(NVS_AES_KEY, runtimeAesKey, AES_KEY_LEN);
  if (loadedLen == AES_KEY_LEN) {
    logInfo("Loaded AES-256 key from NVS.");
    keyStore.end();
    return true;
  }

  for (size_t i = 0; i < AES_KEY_LEN; i += 4) {
    const uint32_t r = esp_random();
    runtimeAesKey[i + 0] = static_cast<uint8_t>(r & 0xFF);
    runtimeAesKey[i + 1] = static_cast<uint8_t>((r >> 8) & 0xFF);
    runtimeAesKey[i + 2] = static_cast<uint8_t>((r >> 16) & 0xFF);
    runtimeAesKey[i + 3] = static_cast<uint8_t>((r >> 24) & 0xFF);
  }

  const size_t savedLen = keyStore.putBytes(NVS_AES_KEY, runtimeAesKey, AES_KEY_LEN);
  keyStore.end();
  if (savedLen != AES_KEY_LEN) {
    logError("Failed to persist generated AES-256 key to NVS.");
    return false;
  }
  logWarn("Generated NEW AES-256 key and saved to NVS.");
  return true;
}

// Собирает plaintext телеметрии, шифрует и формирует JSON для HTTP.
bool buildEncryptedPayload(float tempC, String &cipherB64, String &requestBody) {
  String plain = "{\"temperature_c\":";
  plain += String(tempC, 4);
  plain += ",\"ts_ms\":";
  plain += String(millis());
  plain += "}";
  logInfo("Plain payload: " + plain);

  if (!encryptAesCbcPkcs7(runtimeAesKey,
                          reinterpret_cast<const uint8_t *>(plain.c_str()),
                          plain.length(),
                          cipherB64)) {
    return false;
  }

  requestBody = "{\"device\":\"esp32c6\",\"enc\":\"aes-256-cbc\",\"payload_b64\":\"";
  requestBody += cipherB64;
  requestBody += "\"}";
  return true;
}

// Подключает Wi-Fi с подробными статусами и ограничением времени ожидания.
void connectWiFi() {
  logInfo("Wi-Fi init: STA mode");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  logInfo("Wi-Fi begin. Target SSID: " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  logInfo("Connecting to Wi-Fi...");
  uint32_t attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 60) {
    ++attempt;
    delay(500);
    if (attempt % 10 == 0) {
      String statusLine = "Wi-Fi wait: status=" + String(wifiStatusToText(WiFi.status())) +
                          " (" + String(static_cast<int>(WiFi.status())) + ")";
      logWarn(statusLine);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    logInfo("Wi-Fi connected successfully");
    printWifiDetails();
  } else {
    String failLine = "Wi-Fi connection failed. Status: " + String(wifiStatusToText(WiFi.status())) +
                      " (" + String(static_cast<int>(WiFi.status())) + ")";
    logError(failLine);
  }
}

// Отправляет заранее подготовленный зашифрованный payload на сервер.
// Если Wi-Fi пропал, пробует переподключиться перед POST.
void sendEncryptedTemperature(float tempC, const String &requestBody) {
  if (WiFi.status() != WL_CONNECTED) {
    logWarn("Wi-Fi disconnected, reconnecting...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      logError("Send skipped: Wi-Fi still not connected.");
      return;
    }
  }

  if (String(SERVER_URL).indexOf("YOUR_SERVER") >= 0) {
    logWarn("Send skipped: SERVER_URL is placeholder (YOUR_SERVER).");
    return;
  }

  logInfo("POST to server: " + String(SERVER_URL));
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1200);

  const int code = http.POST(requestBody);
  logInfo("Temp C: " + String(tempC, 4) + " | HTTP code: " + String(code));
  if (code > 0) {
    logInfo("HTTP response: " + http.getString());
  }
  http.end();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  esp_log_level_set(APP_TAG, ESP_LOG_INFO);
  logInfo("=== Boot: ESP32-C6 telemetry node ===");
  logInfo("Configured measure interval (ms): " + String(MEASURE_INTERVAL_MS));
  logInfo("Configured send interval (ms): " + String(SEND_INTERVAL_MS));
  char i2cLine[80];
  snprintf(i2cLine, sizeof(i2cLine), "I2C setup: SDA=%u, SCL=%u, MAX31875 addr=0x%02X",
           I2C_SDA_PIN, I2C_SCL_PIN, MAX31875_ADDR);
  logInfo(String(i2cLine));

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  logInfo("I2C initialized");
  scanI2CBusForTempSensor();
  connectWiFi();

  // Без ключа безопасно шифровать нельзя, поэтому останавливаем устройство.
  if (!loadOrCreateRuntimeAesKey()) {
    logError("Cannot continue without AES-256 key.");
    while (true) {
      delay(1000);
    }
  }

  const String keyHex = bytesToHex(runtimeAesKey, AES_KEY_LEN);
  const String keyB64 = base64Encode(runtimeAesKey, AES_KEY_LEN);
  logWarn("=== AES key (keep private in production) ===");
  logWarn("AES key HEX: " + keyHex);
  logWarn("AES key B64: " + keyB64);
  logInfo("Browser decrypt hint: use AES-CBC with IV=first16bytes(payload_b64)");
}

void loop() {
  const uint32_t now = millis();

  // Цикл измерения: периодическое чтение датчика + немедленное шифрование/лог.
  if (now - lastMeasureMs >= MEASURE_INTERVAL_MS) {
    lastMeasureMs = now;
    ++measureCount;
    float measured = 0.0f;
    String readError;
    logInfo("Reading MAX31875...");
    if (readTemperatureC(measured, readError)) {
      lastMeasuredTempC = measured;
      hasTemperatureSample = true;
      char okLine[96];
      snprintf(okLine, sizeof(okLine), "[MEASURE #%lu] Measured temperature (C): %.4f",
               static_cast<unsigned long>(measureCount), lastMeasuredTempC);
      logInfo(String(okLine));

      logInfo("Encrypting measured payload with AES-256-CBC...");
      if (buildEncryptedPayload(lastMeasuredTempC, lastCipherB64, lastRequestBody)) {
        hasEncryptedPayload = true;
        logInfo("Encrypted payload_b64: " + lastCipherB64);
      } else {
        hasEncryptedPayload = false;
        lastCipherB64 = "";
        lastRequestBody = "";
        logError("AES encryption failed right after measurement.");
      }
    } else {
      char failPrefix[48];
      snprintf(failPrefix, sizeof(failPrefix), "[MEASURE #%lu] Failed to read MAX31875: ",
               static_cast<unsigned long>(measureCount));
      logError(String(failPrefix) + readError);
    }
  }

  // Цикл отправки: использует последний зашифрованный payload со своим интервалом.
  if (hasTemperatureSample && hasEncryptedPayload && (now - lastSendMs >= SEND_INTERVAL_MS)) {
    lastSendMs = now;
    sendEncryptedTemperature(lastMeasuredTempC, lastRequestBody);
  }
  delay(10);
}
