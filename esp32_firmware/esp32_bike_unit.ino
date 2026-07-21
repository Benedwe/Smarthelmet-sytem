/*
  Smart Helmet - Bike Unit firmware
  ---------------------------------
  Hardware on this ESP32:
    - MPU6050 crash/impact sensor
    - SIM800L GSM module
    - GPS module
    - Ignition relay module

  Wireless responsibility:
    - Receives helmet-worn and alcohol status from the Helmet Unit over ESP-NOW.
    - Enables the ignition relay only while a recent safe helmet status is present.

  Android app responsibility:
    - Pair with Bluetooth device "SmartHelmet_Bike_ESP32".
    - Send emergency contacts using the existing line protocol:
      SYNC:Name1|Phone1;Name2|Phone2;...
*/

#include "BluetoothSerial.h"
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <math.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled. Enable Bluetooth in the ESP32 board settings.
#endif

#define RELAY_PIN 5
#define GSM_RX_PIN 16
#define GSM_TX_PIN 17
#define GPS_RX_PIN 26
#define GPS_TX_PIN 27
#define MPU_SDA_PIN 21
#define MPU_SCL_PIN 22

static const uint32_t PROTOCOL_MAGIC = 0x53484D54; // "SHMT"
static const uint8_t PROTOCOL_VERSION = 1;
static const uint8_t MPU6050_ADDRESS = 0x68;
static const uint8_t MAX_CONTACTS = 10;
static const unsigned long HELMET_STATUS_TIMEOUT_MS = 1500;
static const unsigned long ACCIDENT_SMS_COOLDOWN_MS = 30000;
static const float IMPACT_G_THRESHOLD = 3.5f;

struct HelmetStatusPacket {
  uint32_t magic;
  uint8_t version;
  uint32_t sequence;
  bool helmetWorn;
  bool alcoholDetected;
  uint16_t alcoholRaw;
  uint32_t uptimeMs;
};

BluetoothSerial SerialBT;
Preferences prefs;
HardwareSerial gsmSerial(1);
HardwareSerial gpsSerial(2);

String contactNames[MAX_CONTACTS];
String contactPhones[MAX_CONTACTS];
uint8_t contactCount = 0;
String btBuffer = "";
String gpsLine = "";
String lastGpsLocation = "location unavailable";

volatile bool receivedHelmetStatus = false;
HelmetStatusPacket latestHelmetStatus = {};
unsigned long lastHelmetStatusMs = 0;
unsigned long lastSmsSentMs = 0;

bool isSafePhoneNumber(const String &phone) {
  if (phone.length() < 7 || phone.length() > 20) {
    return false;
  }

  for (uint16_t i = 0; i < phone.length(); i++) {
    char c = phone.charAt(i);
    if (!(isDigit(c) || (i == 0 && c == '+'))) {
      return false;
    }
  }
  return true;
}

String sanitizeContactName(String name) {
  name.trim();
  String sanitized = "";
  for (uint16_t i = 0; i < name.length() && sanitized.length() < 32; i++) {
    char c = name.charAt(i);
    if (isAlphaNumeric(c) || c == ' ' || c == '-' || c == '_') {
      sanitized += c;
    }
  }
  sanitized.trim();
  return sanitized;
}

void loadContactsFromFlash() {
  prefs.begin("bike", true);
  contactCount = prefs.getUChar("count", 0);
  if (contactCount > MAX_CONTACTS) {
    contactCount = MAX_CONTACTS;
  }
  for (uint8_t i = 0; i < contactCount; i++) {
    contactNames[i] = prefs.getString(("name" + String(i)).c_str(), "");
    contactPhones[i] = prefs.getString(("phone" + String(i)).c_str(), "");
  }
  prefs.end();
  Serial.printf("Loaded %u emergency contacts.\n", contactCount);
}

void saveContactsToFlash() {
  prefs.begin("bike", false);
  prefs.clear();
  prefs.putUChar("count", contactCount);
  for (uint8_t i = 0; i < contactCount; i++) {
    prefs.putString(("name" + String(i)).c_str(), contactNames[i]);
    prefs.putString(("phone" + String(i)).c_str(), contactPhones[i]);
  }
  prefs.end();
}

bool parseAndStoreContacts(const String &payload) {
  String parsedNames[MAX_CONTACTS];
  String parsedPhones[MAX_CONTACTS];
  uint8_t parsedCount = 0;
  int start = 0;

  while (start < static_cast<int>(payload.length()) && parsedCount < MAX_CONTACTS) {
    int separator = payload.indexOf(';', start);
    String entry = separator == -1 ? payload.substring(start) : payload.substring(start, separator);
    entry.trim();

    if (entry.length() > 0) {
      int bar = entry.indexOf('|');
      if (bar <= 0 || bar == static_cast<int>(entry.length()) - 1) {
        return false;
      }

      String name = sanitizeContactName(entry.substring(0, bar));
      String phone = entry.substring(bar + 1);
      phone.trim();

      if (name.length() == 0 || !isSafePhoneNumber(phone)) {
        return false;
      }

      parsedNames[parsedCount] = name;
      parsedPhones[parsedCount] = phone;
      parsedCount++;
    }

    if (separator == -1) {
      break;
    }
    start = separator + 1;
  }

  if (parsedCount == 0) {
    return false;
  }

  contactCount = parsedCount;
  for (uint8_t i = 0; i < contactCount; i++) {
    contactNames[i] = parsedNames[i];
    contactPhones[i] = parsedPhones[i];
  }
  saveContactsToFlash();
  Serial.printf("Saved %u emergency contacts.\n", contactCount);
  return true;
}

void handleBluetoothData() {
  while (SerialBT.available()) {
    char c = static_cast<char>(SerialBT.read());
    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      if (btBuffer.startsWith("SYNC:")) {
        bool saved = parseAndStoreContacts(btBuffer.substring(5));
        SerialBT.println(saved ? "OK" : "ERR:INVALID_CONTACTS");
      } else {
        SerialBT.println("ERR:UNKNOWN_COMMAND");
      }
      btBuffer = "";
      continue;
    }

    if (btBuffer.length() < 512) {
      btBuffer += c;
    } else {
      btBuffer = "";
      SerialBT.println("ERR:MESSAGE_TOO_LONG");
    }
  }
}

void handleHelmetStatusPacket(const uint8_t *data, int len) {
  if (len != sizeof(HelmetStatusPacket)) {
    return;
  }

  HelmetStatusPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != PROTOCOL_MAGIC || packet.version != PROTOCOL_VERSION) {
    return;
  }

  latestHelmetStatus = packet;
  lastHelmetStatusMs = millis();
  receivedHelmetStatus = true;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onHelmetStatusReceived(const esp_now_recv_info_t *, const uint8_t *data, int len) {
  handleHelmetStatusPacket(data, len);
}
#else
void onHelmetStatusReceived(const uint8_t *, const uint8_t *data, int len) {
  handleHelmetStatusPacket(data, len);
}
#endif

bool initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");
    return false;
  }

  esp_now_register_recv_cb(onHelmetStatusReceived);
  Serial.print("Bike Unit ESP-NOW MAC: ");
  Serial.println(WiFi.macAddress());
  return true;
}

bool initMpu6050() {
  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(0x6B);
  Wire.write(0x00);
  if (Wire.endTransmission(true) != 0) {
    Serial.println("MPU6050 not detected.");
    return false;
  }
  Serial.println("MPU6050 initialized.");
  return true;
}

int16_t readMpuRegister16(uint8_t reg) {
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0;
  }

  Wire.requestFrom(MPU6050_ADDRESS, static_cast<uint8_t>(2), static_cast<uint8_t>(true));
  if (Wire.available() < 2) {
    return 0;
  }

  int16_t high = Wire.read();
  int16_t low = Wire.read();
  return static_cast<int16_t>((high << 8) | low);
}

bool isAccidentDetected() {
  int16_t ax = readMpuRegister16(0x3B);
  int16_t ay = readMpuRegister16(0x3D);
  int16_t az = readMpuRegister16(0x3F);

  float gx = static_cast<float>(ax) / 16384.0f;
  float gy = static_cast<float>(ay) / 16384.0f;
  float gz = static_cast<float>(az) / 16384.0f;
  float magnitude = sqrtf(gx * gx + gy * gy + gz * gz);

  return magnitude >= IMPACT_G_THRESHOLD;
}

String nmeaToDecimalDegrees(const String &raw, const String &hemisphere) {
  if (raw.length() < 4) {
    return "";
  }

  int decimalPoint = raw.indexOf('.');
  int degreeDigits = decimalPoint > 4 ? 3 : 2;
  float degrees = raw.substring(0, degreeDigits).toFloat();
  float minutes = raw.substring(degreeDigits).toFloat();
  float decimal = degrees + (minutes / 60.0f);

  if (hemisphere == "S" || hemisphere == "W") {
    decimal *= -1.0f;
  }

  return String(decimal, 6);
}

void parseGpsSentence(const String &sentence) {
  if (!sentence.startsWith("$GPRMC") && !sentence.startsWith("$GNRMC")) {
    return;
  }

  String fields[12];
  uint8_t fieldCount = 0;
  int start = 0;
  while (fieldCount < 12) {
    int comma = sentence.indexOf(',', start);
    fields[fieldCount++] = comma == -1 ? sentence.substring(start) : sentence.substring(start, comma);
    if (comma == -1) {
      break;
    }
    start = comma + 1;
  }

  if (fieldCount < 7 || fields[2] != "A") {
    return;
  }

  String lat = nmeaToDecimalDegrees(fields[3], fields[4]);
  String lon = nmeaToDecimalDegrees(fields[5], fields[6]);
  if (lat.length() > 0 && lon.length() > 0) {
    lastGpsLocation = "https://maps.google.com/?q=" + lat + "," + lon;
  }
}

void updateGps() {
  while (gpsSerial.available()) {
    char c = static_cast<char>(gpsSerial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      parseGpsSentence(gpsLine);
      gpsLine = "";
    } else if (gpsLine.length() < 128) {
      gpsLine += c;
    } else {
      gpsLine = "";
    }
  }
}

void gsmSendCommand(const String &command, unsigned long waitMs = 500) {
  gsmSerial.println(command);
  delay(waitMs);
  while (gsmSerial.available()) {
    Serial.write(gsmSerial.read());
  }
}

void sendSmsToAllContacts(const String &message) {
  if (contactCount == 0) {
    Serial.println("No emergency contacts saved; SMS skipped.");
    return;
  }

  gsmSendCommand("AT");
  gsmSendCommand("AT+CMGF=1");

  for (uint8_t i = 0; i < contactCount; i++) {
    gsmSerial.print("AT+CMGS=\"");
    gsmSerial.print(contactPhones[i]);
    gsmSerial.println("\"");
    delay(500);
    gsmSerial.print(message);
    gsmSerial.write(26);
    delay(5000);
    Serial.printf("SMS alert sent to %s (%s).\n", contactNames[i].c_str(), contactPhones[i].c_str());
  }
}

bool hasFreshSafeHelmetStatus() {
  if (!receivedHelmetStatus) {
    return false;
  }

  if (millis() - lastHelmetStatusMs > HELMET_STATUS_TIMEOUT_MS) {
    return false;
  }

  return latestHelmetStatus.helmetWorn && !latestHelmetStatus.alcoholDetected;
}

void updateRelay() {
  bool allowIgnition = hasFreshSafeHelmetStatus();
  digitalWrite(RELAY_PIN, allowIgnition ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  initMpu6050();
  initEspNow();

  SerialBT.begin("SmartHelmet_Bike_ESP32");
  Serial.println("Bluetooth SPP started as SmartHelmet_Bike_ESP32.");

  loadContactsFromFlash();
}

void loop() {
  handleBluetoothData();
  updateGps();
  updateRelay();

  if (isAccidentDetected() && (lastSmsSentMs == 0 || millis() - lastSmsSentMs > ACCIDENT_SMS_COOLDOWN_MS)) {
    digitalWrite(RELAY_PIN, LOW);
    String message = "Accident detected! Rider may need help. Location: " + lastGpsLocation;
    sendSmsToAllContacts(message);
    lastSmsSentMs = millis();
  }

  delay(50);
}
