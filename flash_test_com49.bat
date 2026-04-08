@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM -----------------------------
REM Скрипт-помощник ESP32-C6 (меню)
REM -----------------------------
REM 0 - Только сборка проекта
REM 1 - Прошивка готовых бинарников
REM 2 - Открыть монитор порта
REM 3 - Выход

set "BAUD=921600"
set "PROJ_DIR=%~dp0"
set "BOOT_APP0=%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
set "PORT="
set "DO_ERASE="

echo ==========================================
echo ESP32-C6 helper script
echo Project: %PROJ_DIR%
echo ==========================================
echo.

cd /d "%PROJ_DIR%"
if errorlevel 1 (
  echo [ERROR] Failed to switch to project directory.
  pause
  exit /b 1
)

:menu
REM Главный цикл меню.
echo Select action:
echo   [0] Build project
echo   [1] Flash device
echo   [2] Open serial monitor
echo   [3] Exit
choice /C 0123 /N /M "Enter menu number (0/1/2/3): "
set "MENU=%ERRORLEVEL%"

if "%MENU%"=="1" goto :build_only
if "%MENU%"=="2" goto :flash_device
if "%MENU%"=="3" goto :open_monitor
if "%MENU%"=="4" goto :done
goto :menu

:build_step
REM Сборка прошивки и проверка наличия нужных файлов.
echo [BUILD] Building firmware with PlatformIO...
pio run
if errorlevel 1 (
  echo [ERROR] Build failed.
  exit /b 1
)

if not exist ".pio\build\esp32c6\firmware.bin" (
  echo [ERROR] Firmware file not found: .pio\build\esp32c6\firmware.bin
  exit /b 1
)

if not exist "%BOOT_APP0%" (
  echo [ERROR] boot_app0.bin not found:
  echo         %BOOT_APP0%
  exit /b 1
)
exit /b 0

:build_only
call :build_step
if errorlevel 1 (
  pause
  goto :menu
)
echo.
echo [OK] Build completed successfully.
echo.
pause
goto :menu

:select_port
REM Получение списка COM-портов с индексом и описанием.
echo.
echo Detected COM ports:
set "PORT_MAP_FILE=%TEMP%\esp32_ports_%RANDOM%.txt"
python -c "import serial.tools.list_ports as lp; ports=[p for p in lp.comports() if p.device.upper().startswith('COM')]; f=open(r'%PORT_MAP_FILE%','w',encoding='ascii',errors='ignore'); [f.write(f'{i}|{p.device}|{((p.description or \"Unknown device\").encode(\"ascii\",\"ignore\").decode(\"ascii\") or \"Unknown device\")}\n') for i,p in enumerate(ports)]; f.close()"

if not exist "%PORT_MAP_FILE%" (
  echo [ERROR] Failed to enumerate COM ports.
  exit /b 1
)

set "HAS_PORTS=0"
for /f "tokens=1,2,3 delims=|" %%A in (%PORT_MAP_FILE%) do (
  set "HAS_PORTS=1"
  echo   %%A^) %%B - %%C
)

if "!HAS_PORTS!"=="0" (
  echo [WARN] No COM ports found.
  del /q "%PORT_MAP_FILE%" >nul 2>&1
  exit /b 1
)

set "PORT_INDEX="
set /p PORT_INDEX=Select port index (0,1,2...): 
if "%PORT_INDEX%"=="" (
  echo [ERROR] Index is empty.
  del /q "%PORT_MAP_FILE%" >nul 2>&1
  exit /b 1
)

set "PORT="
for /f "tokens=1,2,3 delims=|" %%A in (%PORT_MAP_FILE%) do (
  if "%%A"=="%PORT_INDEX%" set "PORT=%%B"
)
del /q "%PORT_MAP_FILE%" >nul 2>&1

if "%PORT%"=="" (
  echo [ERROR] Invalid index: %PORT_INDEX%
  exit /b 1
)

echo [INFO] Using port: %PORT%
exit /b 0

:flash_step
REM В режиме прошивки предполагается, что бинарники уже собраны (пункт меню 0).
if not exist ".pio\build\esp32c6\firmware.bin" (
  echo [ERROR] Firmware file not found: .pio\build\esp32c6\firmware.bin
  echo         Run menu [0] Build project first.
  exit /b 1
)

if not exist ".pio\build\esp32c6\bootloader.bin" (
  echo [ERROR] Bootloader file not found: .pio\build\esp32c6\bootloader.bin
  echo         Run menu [0] Build project first.
  exit /b 1
)

if not exist ".pio\build\esp32c6\partitions.bin" (
  echo [ERROR] Partitions file not found: .pio\build\esp32c6\partitions.bin
  echo         Run menu [0] Build project first.
  exit /b 1
)

if not exist "%BOOT_APP0%" (
  echo [ERROR] boot_app0.bin not found:
  echo         %BOOT_APP0%
  exit /b 1
)

choice /C YN /N /M "Erase flash memory before programming? (Y/N): "
set "DO_ERASE=%ERRORLEVEL%"

if "%DO_ERASE%"=="1" (
  echo [INFO] Full erase selected.
) else (
  echo [INFO] Keep existing flash/NVS selected.
)

call :select_port
if errorlevel 1 exit /b 1

echo.
echo [FLASH] Target port: %PORT%
echo Closing common serial-monitor processes (if running)...
taskkill /F /IM platformio.exe >nul 2>&1
taskkill /F /IM python.exe >nul 2>&1
taskkill /F /IM pythonw.exe >nul 2>&1
timeout /t 2 /nobreak >nul

if "%DO_ERASE%"=="1" (
  REM Опциональное полное стирание flash; полезно для генерации нового ключа в NVS.
  echo Erasing flash on %PORT%...
  python -m esptool --chip esp32c6 --port %PORT% erase-flash
  if errorlevel 1 (
    echo [WARN] First erase attempt failed.
    echo        If needed, press RST/EN and keep USB connected.
    timeout /t 2 /nobreak >nul
    python -m esptool --chip esp32c6 --port %PORT% erase-flash
    if errorlevel 1 (
      echo [ERROR] Flash erase failed.
      exit /b 1
    )
  )
)

echo First flash attempt...
python -m esptool --chip esp32c6 --port %PORT% --baud %BAUD% --before default-reset --after hard-reset write-flash -z --flash-mode dio --flash-freq 80m --flash-size 4MB 0x0 ".pio\build\esp32c6\bootloader.bin" 0x8000 ".pio\build\esp32c6\partitions.bin" 0xe000 "%BOOT_APP0%" 0x10000 ".pio\build\esp32c6\firmware.bin"
if errorlevel 1 (
  echo [WARN] First flash attempt failed. Retrying once after short delay...
  timeout /t 3 /nobreak >nul
  python -m esptool --chip esp32c6 --port %PORT% --baud %BAUD% --before default-reset --after hard-reset write-flash -z --flash-mode dio --flash-freq 80m --flash-size 4MB 0x0 ".pio\build\esp32c6\bootloader.bin" 0x8000 ".pio\build\esp32c6\partitions.bin" 0xe000 "%BOOT_APP0%" 0x10000 ".pio\build\esp32c6\firmware.bin"
  if errorlevel 1 (
    echo [ERROR] Flash failed. COM port is still busy or unavailable.
    echo         Close serial monitor apps and try again.
    exit /b 1
  )
)
exit /b 0

:flash_device
call :flash_step
if errorlevel 1 (
  pause
  goto :menu
)
echo.
echo [OK] Firmware flashed successfully to %PORT%.
echo You can open monitor with:
echo pio device monitor -p %PORT% -b 115200 --filter printable
echo.
pause
goto :menu

:open_monitor
REM Открывает монитор для выбранного COM-порта.
call :select_port
if errorlevel 1 (
  pause
  goto :menu
)
echo Opening monitor on %PORT%...
pio device monitor -p %PORT% -b 115200 --filter printable
goto :menu

:done
exit /b 0
