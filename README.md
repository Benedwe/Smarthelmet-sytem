# Smart Helmet - Android App + Dual ESP32 Firmware

The system now uses two ESP32 units that communicate wirelessly. The bike unit owns the safety-critical motorcycle hardware, while the helmet unit keeps the rider-facing sensors and audio feedback.

## System Architecture

1. **Android App**: Stores emergency contacts locally and syncs them to the Bike Unit over Bluetooth Classic SPP.
2. **Helmet Unit (ESP32 #1)**: Mounted in the helmet. Contains the IR/wear sensor, MQ-3 alcohol sensor, and DFPlayer/SD audio module.
3. **Bike Unit (ESP32 #2)**: Mounted on the motorcycle. Contains the MPU6050, SIM800L GSM module, GPS module, and ignition relay module.

## Component Placement

| Unit | Components |
| --- | --- |
| Helmet Unit | Helmet ESP32, IR/wear sensor, MQ-3 alcohol sensor, DFPlayer/SD audio module |
| Bike Unit | Bike ESP32, MPU6050, SIM800L GSM module, GPS module, ignition relay module |

```mermaid
flowchart TD
    App[Android App]

    subgraph HelmetUnit[Helmet Unit - ESP32 #1]
        ESP_H[Helmet ESP32]
        IR[IR Sensor - Wear Detection]
        MQ3[MQ-3 Alcohol Sensor]
        Audio[DFPlayer / SD Audio]

        IR --> ESP_H
        MQ3 --> ESP_H
        ESP_H --> Audio
    end

    subgraph BikeUnit[Bike Unit - ESP32 #2]
        ESP_B[Bike ESP32]
        MPU[MPU6050 Crash Sensor]
        GPS[GPS Module - Bike Unit]
        GSM[SIM800L GSM Module - Bike Unit]
        Relay[Ignition Relay - Bike Unit]
        Engine[Motorcycle Engine]

        MPU --> ESP_B
        GPS --> ESP_B
        ESP_B --> GSM
        ESP_B --> Relay
        Relay --> Engine
    end

    App -- "Bluetooth SPP: SYNC contacts" --> ESP_B
    ESP_H -- "ESP-NOW: helmet worn + alcohol status" --> ESP_B
    GSM -- "SMS Alerts" --> Contacts([Emergency Contacts])
```

## Wireless Behavior

- The Helmet Unit broadcasts helmet-worn and alcohol status to the Bike Unit using ESP-NOW.
- The Bike Unit enables the relay only when it has received a recent safe status: helmet worn and alcohol not detected.
- If the wireless status becomes stale, the Bike Unit disables the relay.
- The Android app syncs emergency contacts directly to `SmartHelmet_Bike_ESP32`, because the GSM module is on the Bike Unit.

## Android App (`app/`)

1. Open this folder in Android Studio.
2. Let Gradle sync.
3. Run on a real Android phone. Bluetooth does not work properly on the emulator.
4. Pair the phone with `SmartHelmet_Bike_ESP32` in Android Bluetooth settings.
5. Add emergency contacts in the app.
6. Tap **Sync Contacts to Bike Unit** and select the paired bike ESP32.

The contact sync protocol remains line-based:

```text
SYNC:Name1|Phone1;Name2|Phone2;...
```

The Bike Unit replies with `OK` when contacts are saved.

## Helmet Unit Firmware

File: `esp32_firmware/esp32_helmet_unit.ino`

Default pins:

| Component | ESP32 Pin |
| --- | --- |
| IR / wear sensor | GPIO 4 |
| MQ-3 analog output | GPIO 34 |
| DFPlayer RX | GPIO 18 |
| DFPlayer TX | GPIO 19 |

The sketch:

- Reads helmet-wear status.
- Reads MQ-3 alcohol level.
- Plays DFPlayer warning tracks.
- Sends status packets to the Bike Unit over ESP-NOW.

## Bike Unit Firmware

File: `esp32_firmware/esp32_bike_unit.ino`

Default pins:

| Component | ESP32 Pin |
| --- | --- |
| Relay module | GPIO 5 |
| SIM800L RX/TX | GPIO 16 / GPIO 17 |
| GPS RX/TX | GPIO 26 / GPIO 27 |
| MPU6050 SDA/SCL | GPIO 21 / GPIO 22 |

The sketch:

- Receives helmet safety status over ESP-NOW.
- Controls the ignition relay.
- Detects crash/impact events with MPU6050.
- Reads GPS NMEA location.
- Sends SMS alerts through SIM800L.
- Stores emergency contacts in ESP32 flash.

## Bench Testing Checklist

- Confirm both ESP32 boards print their ESP-NOW MAC addresses in Serial Monitor.
- Confirm the Bike Unit relay stays off when the Helmet Unit is powered off.
- Confirm the relay turns on only when the helmet is worn and alcohol is below threshold.
- Tune `ALCOHOL_THRESHOLD` after MQ-3 warm-up and calibration.
- Tune `IMPACT_G_THRESHOLD` safely on the bench before real-world use.
- Power SIM800L from a stable external supply capable of 2A peak current.
- Test SMS with one contact before adding the full contact list.
