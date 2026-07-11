# Smart Helmet - Emergency Contacts App + ESP32 Firmware

Fully offline: no internet or SIM card needed in the phone. The app talks to
the helmet's ESP32 over Bluetooth Classic (SPP), and the ESP32's own SIM800L
GSM module sends the actual SMS alerts.

## How it fits together

```
[Android App]  --Bluetooth SPP-->  [ESP32]  --serial-->  [SIM800L GSM]  --SMS-->  emergency contacts
   (Room DB:                        (stores contacts
    contact list)                    in flash, runs
                                      sensor logic)
```

## Android app (`app/`)

1. Open the `SmartHelmetApp` folder in Android Studio (Giraffe or newer).
2. Let Gradle sync (it will download Compose, Room, etc.).
3. Run on a phone (min Android 8 / API 26). Requires a real device — Bluetooth
   doesn't work on the emulator.
4. On first launch it asks for Bluetooth permission (and Location on Android
   11 and below, which the OS requires for classic Bluetooth).
5. Add contacts with the form — they save to a local Room database, no
   internet involved.
6. Pair your phone with the ESP32 in **Android's Bluetooth settings first**
   (device name will be `SmartHelmet_ESP32`, default pairing PIN is usually
   `1234` or `0000` for SPP boards).
7. Back in the app, tap **"Sync Contacts to Helmet"** and pick the paired
   ESP32 from the list. The full contact list is sent over Bluetooth.

## ESP32 firmware (`esp32_firmware/esp32_helmet.ino`)

1. Open in Arduino IDE with the ESP32 board package installed.
2. Wire your SIM800L to the pins defined by `GSM_RX_PIN` / `GSM_TX_PIN`
   (default 16/17) — double check your board's UART availability.
3. Flash it. It starts advertising Bluetooth as `SmartHelmet_ESP32`.
4. It listens for the `SYNC:...` message from the app and saves contacts to
   flash (`Preferences`), so they survive reboots/power loss.
5. Fill in the `TODO` sensor-reading functions with your actual wiring and
   calibrated thresholds:
   - `isAccidentDetected()` — MPU6050 impact/tilt logic
   - `isAlcoholDetected()` — MQ-3 threshold
   - `isHelmetWorn()` — wear sensor/switch
   - `getGpsLocationString()` — GPS module parsing
6. When `isAccidentDetected()` returns true, it loops through the stored
   contacts and sends each one an SMS via AT commands to the SIM800L.

## Why Bluetooth Classic (SPP) instead of BLE

For a simple "send a batch of text data" use case like this, classic SPP is
much less code on both ends — it behaves like a plain serial cable. BLE would
require defining GATT services/characteristics, which is unnecessary
complexity here. ESP32 supports both, but SPP is the pragmatic choice.

## Notes / things to double check before relying on this in the field

- Test the accident-detection threshold extensively on the bench — false
  positives/negatives matter a lot for a safety device.
- SIM800L needs a stable 2A-capable power supply (not the ESP32's onboard
  regulator) or it will brown out when transmitting.
- Consider adding a manual "cancel alert" button in case of a false trigger,
  with a short delay before the SMS actually sends.
# Smarthelmet-sytem
