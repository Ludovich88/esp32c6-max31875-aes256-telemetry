# ESP32-C6: защищенная отправка температуры

[English](README.md) | [Русский](README_RU.md)

Проект прошивки для `ESP32-C6` на базе `PlatformIO` + `Arduino`.

Назначение:

- чтение температуры с `MAX31875R0TZS+T` по `I2C` (`SDA=GPIO2`, `SCL=GPIO3`);
- шифрование телеметрии алгоритмом `AES-256-CBC`;
- отправка зашифрованных данных по Wi-Fi на HTTP-сервер;
- подробный лог в консоли (I2C, Wi-Fi, температура, шифрование);
- хранение AES-ключа в `NVS` (генерация при первом запуске).

## Возможности

- **Датчик**: `MAX31875`, адрес задается в `include/secrets.h`.
- **Криптография**: `AES-256-CBC` + `PKCS7`.
- **Формат полезной нагрузки**: `base64(IV[16] + ciphertext)`.
- **Ключ**:
  - первый запуск: генерируется случайный ключ AES-256 и сохраняется в `NVS`;
  - следующие запуски: читается тот же ключ из `NVS`;
  - после `erase flash`: создается новый ключ.
- **Меню-сценарий**: `flash_test_com49.bat` (сборка/прошивка/монитор).

## Структура проекта

- `src/main.cpp` - основная логика прошивки.
- `include/secrets.example.h` - шаблон пользовательских настроек.
- `include/secrets.h` - локальные настройки (в git не попадает).
- `flash_test_com49.bat` - интерактивный helper-скрипт для Windows.
- `platformio.ini` - настройки PlatformIO.

## Настройка

1. Скопируйте `include/secrets.example.h` в `include/secrets.h`.
2. Укажите:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `SERVER_URL`
   - `MEASURE_INTERVAL_MS`
   - `SEND_INTERVAL_MS`
   - `MAX31875_I2C_ADDR` (по умолчанию `0x48`)

## Сборка и прошивка

### Вариант A: команды PlatformIO

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

### Вариант B: меню-скрипт (Windows)

```powershell
.\flash_test_com49.bat
```

Пункты меню:

- `0` - Сборка проекта
- `1` - Прошивка устройства (с вопросом об очистке flash)
- `2` - Открыть монитор порта
- `3` - Выход

## Поведение прошивки

При старте:

1. инициализация I2C;
2. сканирование адресов `0x48..0x4F`;
3. подключение к Wi-Fi и вывод параметров сети;
4. загрузка AES-ключа из `NVS` или генерация нового;
5. вывод ключа в `HEX` и `B64` (для тестовой расшифровки).

В основном цикле:

- каждые `MEASURE_INTERVAL_MS`: чтение температуры, шифрование и лог;
- каждые `SEND_INTERVAL_MS`: отправка последнего зашифрованного payload.

## Формат отправляемых данных

```json
{
  "device": "esp32c6",
  "enc": "aes-256-cbc",
  "payload_b64": "<base64(IV + ciphertext)>"
}
```

Пример исходного plaintext перед шифрованием:

```json
{
  "temperature_c": 34.0000,
  "ts_ms": 8235
}
```

## Пример расшифровки в браузере (WebCrypto)

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

## Важно по безопасности

- Вывод ключа в лог удобен для отладки, но небезопасен для production.
- Для production рекомендуется:
  - отключить вывод ключа;
  - использовать `HTTPS`;
  - рассмотреть `Flash Encryption` / `Secure Boot` на ESP32;
  - внедрить ротацию и защищенное хранение ключей.
