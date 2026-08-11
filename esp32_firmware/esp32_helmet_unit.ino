/*
  Smart Helmet - Helmet Unit firmware
  -----------------------------------
  Hardware on this ESP32:
    - IR sensor / switch for helmet-wear detection
    - MQ-3 alcohol sensor (threshold: 300)
    - DFPlayer Mini / SD audio feedback:
        - Track 1: "Helmet not worn, please wear your helmet"
        - Track 2: "Alcohol detected, engine blocked"

  Wireless responsibility:
    - Sends helmet-worn and alcohol status to the Bike Unit ESP32 over ESP-NOW.
    - The Bike Unit cuts off the ignition relay whenever helmet is not worn OR alcohol > 300.
*/

#include <DFRobotDFPlayerMini.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

#define IR_SENSOR_PIN 4
#define MQ3_SENSOR_PIN 34
#define DF_RX_PIN 18
#define DF_TX_PIN 19
#define ALCOHOL_THRESHOLD 300
#define WIFI_CHANNEL 1

static const uint32_t PROTOCOL_MAGIC = 0x53484D54; // "SHMT"
static const uint8_t PROTOCOL_VERSION = 1;
static const unsigned long STATUS_SEND_INTERVAL_MS = 250;
static const unsigned long AUDIO_ALERT_INTERVAL_MS = 5000;
static const uint8_t ESPNOW_BROADCAST_ADDRESS[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct HelmetStatusPacket {
  uint32_t magic;
  uint8_t version;
  uint32_t sequence;
  bool helmetWorn;
  bool alcoholDetected;
  uint16_t alcoholRaw;
  uint32_t uptimeMs;
};

DFRobotDFPlayerMini dfPlayer;
HardwareSerial dfSerial(2);

uint32_t statusSequence = 0;
unsigned long lastStatusSendMs = 0;
unsigned long lastAudioAlertMs = 0;

bool initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");
    return false;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST_ADDRESS, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  if (!esp_now_is_peer_exist(ESPNOW_BROADCAST_ADDRESS)) {
    esp_err_t addPeerResult = esp_now_add_peer(&peerInfo);
    if (addPeerResult != ESP_OK) {
      Serial.printf("Failed to register ESP-NOW broadcast peer: %d\n", addPeerResult);
      return false;
    }
  }

  Serial.printf("Helmet Unit ESP-NOW MAC: %s (Channel %d)\n", WiFi.macAddress().c_str(), WIFI_CHANNEL);
  return true;
}

bool isHelmetWorn() {
  // Most IR obstacle modules are LOW when an object is detected (helmet worn).
  return digitalRead(IR_SENSOR_PIN) == LOW;
}

uint16_t readAlcoholRaw() {
  return static_cast<uint16_t>(analogRead(MQ3_SENSOR_PIN));
}

bool isAlcoholDetected(uint16_t alcoholRaw) {
  return alcoholRaw > ALCOHOL_THRESHOLD;
}

void sendHelmetStatus(bool helmetWorn, bool alcoholDetected, uint16_t alcoholRaw) {
  HelmetStatusPacket packet = {
    PROTOCOL_MAGIC,
    PROTOCOL_VERSION,
    ++statusSequence,
    helmetWorn,
    alcoholDetected,
    alcoholRaw,
    millis()
  };

  esp_err_t result = esp_now_send(ESPNOW_BROADCAST_ADDRESS, reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  if (result != ESP_OK) {
    Serial.printf("Failed to send helmet status over ESP-NOW: %d\n", result);
  }
}

void handleAudioAlerts(bool helmetWorn, bool alcoholDetected, uint16_t alcoholRaw) {
  if (helmetWorn && !alcoholDetected) {
    return;
  }

  unsigned long now = millis();
  if (now - lastAudioAlertMs < AUDIO_ALERT_INTERVAL_MS) {
    return;
  }

  if (!helmetWorn) {
    Serial.println("Helmet not worn! Playing voice alert: 'Helmet not worn, please wear your helmet'");
    dfPlayer.play(1); // Track 1: "Helmet not worn, please wear your helmet"
  } else if (alcoholDetected) {
    Serial.printf("High alcohol level detected (%u > 300)! Playing voice alert: 'Alcohol detected, engine blocked'\n", alcoholRaw);
    dfPlayer.play(2); // Track 2: "Alcohol detected, engine blocked"
  }

  lastAudioAlertMs = now;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(IR_SENSOR_PIN, INPUT);
  pinMode(MQ3_SENSOR_PIN, INPUT);

  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  if (!dfPlayer.begin(dfSerial)) {
    Serial.println("DFPlayer Mini failed to initialize. Audio alerts disabled.");
  } else {
    dfPlayer.volume(20);
    Serial.println("DFPlayer Mini initialized.");
  }

  initEspNow();
}

void loop() {
  uint16_t alcoholRaw = readAlcoholRaw();
  bool helmetWorn = isHelmetWorn();
  bool alcoholDetected = isAlcoholDetected(alcoholRaw);

  handleAudioAlerts(helmetWorn, alcoholDetected, alcoholRaw);

  unsigned long now = millis();
  if (now - lastStatusSendMs >= STATUS_SEND_INTERVAL_MS) {
    sendHelmetStatus(helmetWorn, alcoholDetected, alcoholRaw);
    lastStatusSendMs = now;
  }

  delay(25);
}

