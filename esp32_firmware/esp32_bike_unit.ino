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
    - Send emergency contacts using the line protocol:
      SYNC:Name1|Phone1;Name2|Phone2;...
*/

#include "BluetoothSerial.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <esp_now.h>
#include <math.h>

#define WIFI_CHANNEL 1

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

bool hasGpsFix = false;
unsigned long lastGpsFixMs = 0;
String lastGpsLat = "";
String lastGpsLon = "";
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
  String trimmed = payload;
  trimmed.trim();
  if (trimmed.length() == 0) {
    contactCount = 0;
    saveContactsToFlash();
    Serial.println("Cleared all emergency contacts.");
    return true;
  }

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
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");
    return false;
  }

  esp_now_register_recv_cb(onHelmetStatusReceived);
  Serial.printf("Bike Unit ESP-NOW MAC: %s (Channel %d)\n", WiFi.macAddress().c_str(), WIFI_CHANNEL);
  return true;
}

bool initMpu6050() {
  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
  
  // Power management 1: wake up device
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(0x6B);
  Wire.write(0x00);
  if (Wire.endTransmission(true) != 0) {
    Serial.println("MPU6050 not detected.");
    return false;
  }

  // ACCEL_CONFIG register 0x1C: set ±8g range (AFS_SEL=2, 4096 LSB/g)
  Wire.beginTransmission(MPU6050_ADDRESS);
  Wire.write(0x1C);
  Wire.write(0x10);
  Wire.endTransmission(true);

  Serial.println("MPU6050 initialized (±8g full-scale range).");
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

  // Scaled for ±8g range: 4096 LSB per g
  float gx = static_cast<float>(ax) / 4096.0f;
  float gy = static_cast<float>(ay) / 4096.0f;
  float gz = static_cast<float>(az) / 4096.0f;
  float magnitude = sqrtf(gx * gx + gy * gy + gz * gz);

  return magnitude >= IMPACT_G_THRESHOLD;
}

String nmeaToDecimalDegrees(const String &raw, const String &hemisphere) {
  if (raw.length() < 4) {
    return "";
  }

  int decimalPoint = raw.indexOf('.');
  if (decimalPoint == -1) {
    return "";
  }

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
  String cleanSentence = sentence;
  cleanSentence.trim();
  if (cleanSentence.length() == 0) {
    return;
  }

  String fields[15];
  uint8_t fieldCount = 0;
  int start = 0;
  while (fieldCount < 15) {
    int comma = cleanSentence.indexOf(',', start);
    fields[fieldCount++] = comma == -1 ? cleanSentence.substring(start) : cleanSentence.substring(start, comma);
    if (comma == -1) {
      break;
    }
    start = comma + 1;
  }

  String latStr = "";
  String lonStr = "";
  bool validFix = false;

  // Handle RMC sentence ($GPRMC or $GNRMC)
  if (fields[0].endsWith("RMC")) {
    // fields[2] is Status: "A" = Active/Valid, "V" = Void/Invalid
    if (fieldCount >= 7 && fields[2] == "A") {
      latStr = nmeaToDecimalDegrees(fields[3], fields[4]);
      lonStr = nmeaToDecimalDegrees(fields[5], fields[6]);
      validFix = (latStr.length() > 0 && lonStr.length() > 0);
    }
  }
  // Handle GGA sentence ($GPGGA or $GNGGA)
  else if (fields[0].endsWith("GGA")) {
    // fields[6] is Fix quality: "0" = Invalid, "1"/"2" = Valid
    if (fieldCount >= 7 && fields[6] != "0" && fields[6].length() > 0) {
      latStr = nmeaToDecimalDegrees(fields[2], fields[3]);
      lonStr = nmeaToDecimalDegrees(fields[4], fields[5]);
      validFix = (latStr.length() > 0 && lonStr.length() > 0);
    }
  }

  if (validFix) {
    hasGpsFix = true;
    lastGpsFixMs = millis();
    lastGpsLat = latStr;
    lastGpsLon = lonStr;
    lastGpsLocation = "https://maps.google.com/?q=" + latStr + "," + lonStr;
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

String gsmSendCommand(const String &command, unsigned long waitMs = 500) {
  gsmSerial.println(command);
  delay(waitMs);
  String response = "";
  while (gsmSerial.available()) {
    char c = static_cast<char>(gsmSerial.read());
    response += c;
    Serial.write(c);
  }
  return response;
}

bool ensureGsmNetworkRegistered() {
  String resp = gsmSendCommand("AT+CREG?", 400);
  if (resp.indexOf(",1") != -1 || resp.indexOf(",5") != -1) {
    return true;
  }
  
  // Wait up to 3 seconds if GSM is searching for network
  unsigned long start = millis();
  while (millis() - start < 3000) {
    delay(500);
    resp = gsmSendCommand("AT+CREG?", 400);
    if (resp.indexOf(",1") != -1 || resp.indexOf(",5") != -1) {
      return true;
    }
  }
  return false;
}

bool initGsm() {
  Serial.println("Initializing SIM800 GSM module...");
  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(500);

  // Sync baud rate
  bool ready = false;
  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    String resp = gsmSendCommand("AT", 300);
    if (resp.indexOf("OK") != -1) {
      ready = true;
      break;
    }
    delay(300);
  }

  if (!ready) {
    Serial.println("SIM800 GSM module not responding to AT commands.");
    return false;
  }

  gsmSendCommand("ATE0", 300);            // Turn off command echo
  gsmSendCommand("AT+CMGF=1", 300);       // Set SMS to text mode
  gsmSendCommand("AT+CSCS=\"GSM\"", 300); // Set character set
  gsmSendCommand("AT+CPIN?", 500);        // Check SIM PIN status
  ensureGsmNetworkRegistered();

  Serial.println("SIM800 GSM module initialized.");
  return true;
}

bool sendSmsToSingleContact(const String &name, const String &phone, const String &message) {
  Serial.printf("Sending SMS to %s (%s)...\n", name.c_str(), phone.c_str());

  // Flush rx buffer
  while (gsmSerial.available()) {
    gsmSerial.read();
  }

  gsmSerial.print("AT+CMGS=\"");
  gsmSerial.print(phone);
  gsmSerial.println("\"");

  // Wait for '>' prompt from SIM800 module
  bool gotPrompt = false;
  unsigned long promptStart = millis();
  while (millis() - promptStart < 3000) {
    if (gsmSerial.available()) {
      char c = static_cast<char>(gsmSerial.read());
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    delay(10);
  }

  if (!gotPrompt) {
    Serial.printf("SIM800 prompt '>' timed out for contact %s.\n", name.c_str());
    gsmSerial.write(27); // Send ESC to cancel AT+CMGS
    delay(500);
    return false;
  }

  // Send SMS body and terminating CTRL+Z
  gsmSerial.print(message);
  gsmSerial.write(26);

  // Wait for OK or +CMGS response
  bool sentOk = false;
  unsigned long sendStart = millis();
  String resp = "";
  while (millis() - sendStart < 10000) {
    while (gsmSerial.available()) {
      char c = static_cast<char>(gsmSerial.read());
      resp += c;
      if (resp.indexOf("OK") != -1 || resp.indexOf("+CMGS:") != -1) {
        sentOk = true;
        break;
      }
    }
    if (sentOk) {
      break;
    }
    delay(50);
  }

  if (sentOk) {
    Serial.printf("SMS alert successfully sent to %s (%s).\n", name.c_str(), phone.c_str());
  } else {
    Serial.printf("SMS delivery confirmation timed out for %s (%s).\n", name.c_str(), phone.c_str());
  }

  return sentOk;
}

void sendSmsToAllContacts(const String &message) {
  if (contactCount == 0) {
    Serial.println("No emergency contacts saved; SMS skipped.");
    return;
  }

  ensureGsmNetworkRegistered();
  gsmSendCommand("AT+CMGF=1", 300); // Ensure text mode

  for (uint8_t i = 0; i < contactCount; i++) {
    bool success = sendSmsToSingleContact(contactNames[i], contactPhones[i], message);
    if (!success) {
      Serial.printf("Retrying SMS dispatch to %s (%s)...\n", contactNames[i].c_str(), contactPhones[i].c_str());
      delay(1000);
      sendSmsToSingleContact(contactNames[i], contactPhones[i], message);
    }
    delay(1000);
  }
}

String buildEmergencyAlertMessage() {
  String msg = "EMERGENCY ALERT: Smart Helmet detected an accident! Rider needs assistance. ";
  
  if (hasGpsFix) {
    unsigned long ageSec = (millis() - lastGpsFixMs) / 1000;
    if (ageSec < 60) {
      msg += "Location: " + lastGpsLocation;
    } else {
      msg += "Last known location (" + String(ageSec) + "s ago): " + lastGpsLocation;
    }
  } else {
    msg += "Location: GPS fix pending. Please contact rider immediately!";
  }
  
  return msg;
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

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  initMpu6050();
  initGsm();
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
    updateGps(); // Drain any pending GPS serial bytes for the latest location
    String message = buildEmergencyAlertMessage();
    sendSmsToAllContacts(message);
    lastSmsSentMs = millis();
  }

  delay(50);
}

