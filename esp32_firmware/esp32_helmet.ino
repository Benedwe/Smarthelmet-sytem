/*
  Smart Helmet - ESP32 firmware skeleton
  ---------------------------------------
  Responsibilities:
   1. Accept a paired Bluetooth Classic (SPP) connection from the Android app
      and receive the emergency contact list, storing it in flash (Preferences)
      so it survives power loss.
   2. Read sensors: MQ-3 (alcohol), MPU6050 (accident / impact), a helmet-wear
      switch/IR sensor, and a GPS module (for location in the alert SMS).
   3. On accident detection, send an SMS via SIM800L to every stored contact.

  Wire protocol from the app (see HelmetBluetoothManager.kt):
      "SYNC:Name1|Phone1;Name2|Phone2;...\n"
  Firmware replies "OK\n" once contacts are saved.

  NOTE: This is a structural skeleton meant to be filled in with your specific
  sensor wiring/pins and thresholds. Sensor threshold tuning (what counts as
  an "accident" from accelerometer data, alcohol ppm cutoff, etc.) depends on
  your hardware calibration and should be tested safely on the bench before
  relying on it.
*/

#include "BluetoothSerial.h"
#include <Preferences.h>
#include <DFRobotDFPlayerMini.h>

#define IR_SENSOR_PIN 4
#define MQ3_SENSOR_PIN 34
#define RELAY_PIN 5
#define DF_RX_PIN 18
#define DF_TX_PIN 19
#define ALCOHOL_THRESHOLD 2000

DFRobotDFPlayerMini myDFPlayer;
HardwareSerial dfSerial(2);

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Enable it in "Tools -> Board" menu settings.
#endif

BluetoothSerial SerialBT;
Preferences prefs;

// SIM800L is a serial GSM modem - connect its TX/RX to a free ESP32 UART.
// Adjust pins to match your wiring.
#define GSM_RX_PIN 16
#define GSM_TX_PIN 17
HardwareSerial gsmSerial(1); // UART1

const int MAX_CONTACTS = 10;
String contactNames[MAX_CONTACTS];
String contactPhones[MAX_CONTACTS];
int contactCount = 0;

// ---------- Contact storage ----------

void loadContactsFromFlash() {
  prefs.begin("helmet", true); // read-only
  contactCount = prefs.getInt("count", 0);
  for (int i = 0; i < contactCount; i++) {
    contactNames[i] = prefs.getString(("name" + String(i)).c_str(), "");
    contactPhones[i] = prefs.getString(("phone" + String(i)).c_str(), "");
  }
  prefs.end();
  Serial.printf("Loaded %d contacts from flash\n", contactCount);
}

void saveContactsToFlash() {
  prefs.begin("helmet", false); // read-write
  prefs.putInt("count", contactCount);
  for (int i = 0; i < contactCount; i++) {
    prefs.putString(("name" + String(i)).c_str(), contactNames[i]);
    prefs.putString(("phone" + String(i)).c_str(), contactPhones[i]);
  }
  prefs.end();
}

// Parses "Name1|Phone1;Name2|Phone2;..." into the contact arrays
void parseAndStoreContacts(const String &payload) {
  contactCount = 0;
  int start = 0;
  while (start < (int)payload.length() && contactCount < MAX_CONTACTS) {
    int sep = payload.indexOf(';', start);
    String entry = (sep == -1) ? payload.substring(start) : payload.substring(start, sep);
    int bar = entry.indexOf('|');
    if (bar != -1) {
      contactNames[contactCount] = entry.substring(0, bar);
      contactPhones[contactCount] = entry.substring(bar + 1);
      contactCount++;
    }
    if (sep == -1) break;
    start = sep + 1;
  }
  saveContactsToFlash();
  Serial.printf("Saved %d contacts\n", contactCount);
}

// ---------- Bluetooth receive handling ----------

String btBuffer = "";

void handleBluetoothData() {
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n') {
      if (btBuffer.startsWith("SYNC:")) {
        String payload = btBuffer.substring(5);
        parseAndStoreContacts(payload);
        SerialBT.println("OK");
      }
      btBuffer = "";
    } else {
      btBuffer += c;
    }
  }
}

// ---------- GSM SMS sending ----------

void gsmSendCommand(const String &cmd, unsigned long waitMs = 500) {
  gsmSerial.println(cmd);
  delay(waitMs);
  while (gsmSerial.available()) {
    Serial.write(gsmSerial.read()); // echo modem response to Serial for debugging
  }
}

void sendSmsToAllContacts(const String &message) {
  for (int i = 0; i < contactCount; i++) {
    gsmSendCommand("AT+CMGF=1"); // text mode
    gsmSerial.print("AT+CMGS=\"");
    gsmSerial.print(contactPhones[i]);
    gsmSerial.println("\"");
    delay(300);
    gsmSerial.print(message);
    gsmSerial.write(26); // Ctrl+Z sends the SMS
    delay(3000);
    Serial.printf("SMS sent to %s (%s)\n", contactNames[i].c_str(), contactPhones[i].c_str());
  }
}

// ---------- Sensor stubs (fill in with your calibrated logic) ----------

bool isAccidentDetected() {
  // TODO: read MPU6050, check for sudden deceleration / impact / tilt pattern
  return false;
}

bool isAlcoholDetected() {
  int alcoholValue = analogRead(MQ3_SENSOR_PIN);
  return alcoholValue > ALCOHOL_THRESHOLD;
}

bool isHelmetWorn() {
  // IR sensor logic: typically LOW when object is detected (helmet is worn)
  return digitalRead(IR_SENSOR_PIN) == LOW;
}

String getGpsLocationString() {
  // TODO: read from GPS module (e.g. NEO-6M) and format as
  // "https://maps.google.com/?q=LAT,LON"
  return "location unavailable";
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  gsmSerial.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

  pinMode(IR_SENSOR_PIN, INPUT);
  pinMode(MQ3_SENSOR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Default off

  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  if (!myDFPlayer.begin(dfSerial)) {
    Serial.println("DFPlayer mini failed to initialize.");
  } else {
    Serial.println("DFPlayer mini initialized.");
    myDFPlayer.volume(20);  // Set volume value. From 0 to 30
  }

  SerialBT.begin("SmartHelmet_ESP32"); // Bluetooth device name shown when pairing
  Serial.println("Bluetooth SPP started, pair with 'SmartHelmet_ESP32'");

  loadContactsFromFlash();
}

void loop() {
  handleBluetoothData();

  bool helmetWorn = isHelmetWorn();
  bool alcoholDetected = isAlcoholDetected();

  // Ignition control logic
  if (helmetWorn && !alcoholDetected) {
    digitalWrite(RELAY_PIN, HIGH); // Allow ignition
  } else {
    digitalWrite(RELAY_PIN, LOW); // Stop engine
    
    // Play voice alerts using DFPlayer (alert every 5 seconds)
    static unsigned long lastAlertTime = 0;
    if (millis() - lastAlertTime > 5000) { 
      if (!helmetWorn) {
        Serial.println("Helmet not worn! Engine off.");
        myDFPlayer.play(1); // Assuming track 1 is "Wear Helmet"
      } else if (alcoholDetected) {
        Serial.println("Alcohol detected! Engine off.");
        myDFPlayer.play(2); // Assuming track 2 is "Alcohol Detected"
      }
      lastAlertTime = millis();
    }
  }

  if (isAccidentDetected()) {
    String msg = "Accident detected! Rider may need help. Location: " + getGpsLocationString();
    sendSmsToAllContacts(msg);
    delay(30000); // avoid spamming repeated SMS for the same event
  }

  delay(200);
}
