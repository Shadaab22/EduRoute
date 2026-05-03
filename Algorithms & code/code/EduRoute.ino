#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include <time.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ======== MODE SELECT PIN ========
const int MODE_PIN = 16; // switch to GND -> LOW, else pulled-up -> HIGH

// ======== SHARED DEFINITIONS ========
const char* WIFI_SSID     = "XXXXXXXXXX";
const char* WIFI_PASSWORD = "XXXXXXXXXX";

const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/macros/s/AKfycbx8vkiwSCcLnZrbwufegJQVbc9cHxvfCcDUuD9XnxUoDnU2IjEEoRQp4QLbWyoMSUn3tA/exec";

const char* TELEGRAM_BOT_TOKEN =
  "8251394432:AAFwvJP9_t2XX5CD2ND2CML8VVXdVhHjDo8";

// Fingerprint / GPS / Buzzer pins
#define FINGER_RX 25
#define FINGER_TX 26
#define GPS_RX    4
#define GPS_TX    5
#define BUZZER_PIN 14

// LCD: adjust address 0x27 if needed
LiquidCrystal_I2C lcd(0x27, 16, 2);

// HardwareSerial for fingerprint and GPS
HardwareSerial fpSerial(2);   // RX=25, TX=26
HardwareSerial gpsSerial(1);  // RX=4,  TX=5

Adafruit_Fingerprint finger(&fpSerial);
TinyGPSPlus gps;

// ---------- LCD helper ----------
void lcdShow(const String &line1, const String &line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

// ---------- Common helpers ----------
String urlencode(const String& str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if ((c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      char code0 = (c >> 4) & 0xF;
      char code1 = c & 0xF;
      encoded += char(code0 > 9 ? code0 + 'A' - 10 : code0 + '0');
      encoded += char(code1 > 9 ? code1 + 'A' - 10 : code1 + '0');
    }
  }
  return encoded;
}

String getTimestampIST() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 1000)) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d IST",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
  }
  if (gps.time.isValid()) {
    int hourIST   = gps.time.hour() + 5;
    int minuteIST = gps.time.minute() + 30;
    int secondIST = gps.time.second();

    if (minuteIST >= 60) {
      minuteIST -= 60;
      hourIST += 1;
    }
    if (hourIST >= 24) hourIST -= 24;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d IST",
             hourIST, minuteIST, secondIST);
    return String(buf);
  }
  return String("time not available");
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("⚠ WiFi disconnected, reconnecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connected/reconnected");
  } else {
    Serial.println("❌ WiFi reconnect failed.");
  }
}

// ========== STEP 1: ADMIN MODE (Enrollment) ==========

struct FingerData {
  int    id;
  String name;
  String busNo;
  String parent;
  String parentChatId;
};

FingerData fingerList[127];
int enrolledCount = 0;

void updateGoogleSheet(String action, int id, String name,
                       String busNo = "", String parent = "", String parentChatId = "") {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi not connected. Cannot update Google Sheet.");
    lcdShow("Admin:", "WiFi Error");
    return;
  }

  HTTPClient http;
  http.begin(GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String postData = "action=" + urlencode(action) +
                    "&id=" + String(id) +
                    "&name=" + urlencode(name) +
                    "&busNo=" + urlencode(busNo) +
                    "&parent=" + urlencode(parent) +
                    "&parentChatId=" + urlencode(parentChatId);

  Serial.println("🌐 Sending POST data:");
  Serial.println(postData);

  int httpResponseCode = http.POST(postData);
  String response = http.getString();

  if (httpResponseCode == 200) {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, response);
    if (!error && doc.containsKey("status")) {
      Serial.print("📝 Google Sheets status: ");
      Serial.println(doc["status"].as<const char*>());
    } else {
      Serial.println("✅ Google Sheet updated (no JSON status field).");
    }
  } else {
    Serial.printf("❌ HTTP error: %d\n", httpResponseCode);
    Serial.printf(">> %s\n", response.c_str());
  }
  http.end();
  delay(300);
}

void syncFromGoogleSheet() {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi not connected. Cannot sync from Google Sheet.");
    lcdShow("Admin:", "WiFi Error");
    return;
  }

  HTTPClient http;
  String url = String(GOOGLE_SCRIPT_URL) + "?action=list";
  Serial.println("🌐 Requesting list from Google Sheet...");
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpResponseCode = http.GET();

  if (httpResponseCode == 200) {
    String payload = http.getString();
    Serial.println("✅ Sync payload:");
    Serial.println(payload);

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("❌ JSON error: ");
      Serial.println(error.c_str());
      http.end();
      return;
    }
    if (!doc.is<JsonArray>()) {
      Serial.println("❌ Expected JSON array.");
      http.end();
      return;
    }

    for (int i = 0; i < 127; i++) {
      fingerList[i].id = 0;
      fingerList[i].name = "";
      fingerList[i].busNo = "";
      fingerList[i].parent = "";
      fingerList[i].parentChatId = "";
    }
    enrolledCount = 0;

    for (JsonObject obj : doc.as<JsonArray>()) {
      int id         = obj["ID"]     | 0;
      String name    = String(obj["Name"]   | "");
      String busNo   = obj["BusNo"].as<String>();
      String parent  = obj["Parent"].as<String>();
      String parentChatId = obj["ParentChatId"].as<String>();

      if (id >= 1 && id <= 127 && name.length() > 0) {
        fingerList[id - 1].id           = id;
        fingerList[id - 1].name         = name;
        fingerList[id - 1].busNo        = busNo;
        fingerList[id - 1].parent       = parent;
        fingerList[id - 1].parentChatId = parentChatId;
        enrolledCount++;
      }
    }
    Serial.printf("✅ Local list updated from Google Sheets. Total enrolled: %d\n", enrolledCount);

    for (int i = 0; i < 5; i++) {
      if (fingerList[i].id != 0) {
        Serial.print("Cache -> ID: ");
        Serial.print(fingerList[i].id);
        Serial.print(", Name: ");
        Serial.print(fingerList[i].name);
        Serial.print(", Bus: ");
        Serial.print(fingerList[i].busNo);
        Serial.print(", Parent: ");
        Serial.print(fingerList[i].parent);
        Serial.print(", ParentChatId: ");
        Serial.println(fingerList[i].parentChatId);
      }
    }
  } else {
    Serial.printf("❌ Sync failed. HTTP Code: %d\n", httpResponseCode);
    Serial.println(http.getString());
  }
  http.end();
  delay(300);
}

void showMenu() {
  Serial.println("\n------------------------------------------");
  Serial.println("1️⃣  Enroll new fingerprint");
  Serial.println("2️⃣  Verify fingerprint");
  Serial.println("3️⃣  Delete fingerprint by ID");
  Serial.println("4️⃣  Delete ALL fingerprints");
  Serial.println("5️⃣  List all enrolled fingerprints");
  Serial.println("------------------------------------------");
  Serial.print("➡  Enter option: ");
}

void enrollFingerprint() {
  Serial.println("\nEnter ID (1–127): ");
  lcdShow("Admin:", "Enter ID");
  while (Serial.available() == 0);
  int id = Serial.parseInt();
  while (Serial.available()) Serial.read();

  if (id < 1 || id > 127) {
    Serial.println("❌ Invalid ID! Must be between 1–127.");
    lcdShow("Admin:", "Invalid ID");
    return;
  }
  if (fingerList[id - 1].id != 0) {
    Serial.println("⚠  This ID is already in use. Try another one.");
    lcdShow("Admin:", "ID In Use");
    return;
  }

  Serial.println("Enter name (A-Z, a-z, 0-9, space): ");
  lcdShow("Admin:", "Enter Name");
  while (Serial.available() == 0);
  String name = Serial.readStringUntil('\n');
  name.trim();

  for (int i = 0; i < name.length(); i++) {
    char c = name.charAt(i);
    if (!(isalnum(c) || c == ' ')) {
      Serial.println("❌ Invalid name! Only English letters, numbers, and spaces allowed.");
      lcdShow("Admin:", "Name Invalid");
      return;
    }
  }

  for (int i = 0; i < 127; i++) {
    if (fingerList[i].name.equalsIgnoreCase(name) && fingerList[i].id != 0) {
      Serial.println("⚠  This name already exists! Use a unique one.");
      lcdShow("Admin:", "Name Exists");
      return;
    }
  }

  Serial.println("Enter Bus No: ");
  lcdShow("Admin:", "Enter Bus No");
  while (Serial.available() == 0);
  String busNo = Serial.readStringUntil('\n');
  busNo.trim();

  Serial.println("Enter Parent contact: ");
  lcdShow("Admin:", "Parent Contact");
  while (Serial.available() == 0);
  String parent = Serial.readStringUntil('\n');
  parent.trim();

  Serial.println("Enter Parent chat id (Telegram): ");
  lcdShow("Admin:", "Parent Chat ID");
  while (Serial.available() == 0);
  String parentChatId = Serial.readStringUntil('\n');
  parentChatId.trim();

  Serial.printf("🧩 Enrolling ID #%d (%s)\n", id, name.c_str());
  delay(800);

  Serial.println("👉 Place your finger on the sensor...");
  lcdShow("Admin:", "Place Finger");
  while (finger.getImage() != FINGERPRINT_OK);
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println("❌ Error capturing image!");
    lcdShow("Admin:", "Image Error");
    return;
  }

  Serial.println("✋ Remove your finger...");
  lcdShow("Admin:", "Remove Finger");
  delay(2000);

  Serial.println("👉 Place the same finger again...");
  lcdShow("Admin:", "Place Again");
  while (finger.getImage() != FINGERPRINT_OK);
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    Serial.println("❌ Error capturing second image!");
    lcdShow("Admin:", "Image Error");
    return;
  }

  if (finger.fingerFastSearch() == FINGERPRINT_OK) {
    Serial.println("⚠  Fingerprint already enrolled! Use a different finger.");
    lcdShow("Admin:", "Finger Exists");
    return;
  }

  if (finger.createModel() == FINGERPRINT_OK) {
    if (finger.storeModel(id) == FINGERPRINT_OK) {
      Serial.printf("✅ Fingerprint saved for %s (ID: %d)\n", name.c_str(), id);
      fingerList[id - 1].id           = id;
      fingerList[id - 1].name         = name;
      fingerList[id - 1].busNo        = busNo;
      fingerList[id - 1].parent       = parent;
      fingerList[id - 1].parentChatId = parentChatId;
      enrolledCount++;

      updateGoogleSheet("enroll", id, name, busNo, parent, parentChatId);

      lcdShow("Admin:", "Student Regd"); // student info registered
    } else {
      Serial.println("❌ Failed to store fingerprint in sensor.");
      lcdShow("Admin:", "Store Failed");
    }
  } else {
    Serial.println("❌ Fingerprints did not match!");
    lcdShow("Admin:", "Enroll Failed");
  }
  delay(800);
}

void verifyFingerprintAdmin() {
  if (enrolledCount == 0) {
    Serial.println("⚠  No fingerprints enrolled yet!");
    lcdShow("Admin:", "No Students");
    return;
  }

  Serial.println("\n👉 Place your finger on the sensor...");
  lcdShow("Admin:", "Place Finger");
  unsigned long start = millis();
  int id = -1;

  while (millis() - start < 7000) {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      if (finger.image2Tz() == FINGERPRINT_OK && finger.fingerFastSearch() == FINGERPRINT_OK) {
        id = finger.fingerID;
        break;
      }
    }
    delay(200);
  }

  if (id >= 0 && id <= 127 && fingerList[id - 1].id != 0) {
    Serial.println("✅ Match found!");
    Serial.print("   ID: ");   Serial.println(id);
    Serial.print("   Name: "); Serial.println(fingerList[id - 1].name);
    Serial.print("   Bus No: "); Serial.println(fingerList[id - 1].busNo);
    Serial.print("   Parent: "); Serial.println(fingerList[id - 1].parent);
    Serial.print("   Parent chat id: "); Serial.println(fingerList[id - 1].parentChatId);
    lcdShow("Admin:", "Student Found");
  } else {
    Serial.println("⏱ Timeout or no match found.");
    lcdShow("Admin:", "No Match");
  }
  delay(800);
}

void deleteFingerprintByID() {
  if (enrolledCount == 0) {
    Serial.println("⚠ No fingerprints available to delete!");
    lcdShow("Admin:", "No Students");
    return;
  }

  Serial.println("\nEnter the fingerprint ID (1–127) to delete: ");
  lcdShow("Admin:", "Enter ID Del");
  while (Serial.available() == 0);
  int id = Serial.parseInt();
  while (Serial.available()) Serial.read();

  if (id < 1 || id > 127) {
    Serial.println("❌ Invalid ID range!");
    lcdShow("Admin:", "Invalid ID");
    return;
  }
  if (fingerList[id - 1].id == 0) {
    Serial.println("⚠ No fingerprint found with that ID!");
    lcdShow("Admin:", "ID Empty");
    return;
  }

  String nameToDelete = fingerList[id - 1].name;
  Serial.printf("🗑 Deleting fingerprint ID %d (%s)...\n", id, nameToDelete.c_str());
  lcdShow("Admin:", "Deleting...");
  delay(800);

  uint8_t result = finger.deleteModel(id);
  if (result == FINGERPRINT_OK) {
    Serial.println("✅ Fingerprint deleted from sensor.");

    updateGoogleSheet("delete", id, nameToDelete);
    delay(300);

    fingerList[id - 1].id = 0;
    fingerList[id - 1].name = "";
    fingerList[id - 1].busNo = "";
    fingerList[id - 1].parent = "";
    fingerList[id - 1].parentChatId = "";
    enrolledCount--;

    Serial.println("✅ Record removed locally and from Google Sheet.");
    lcdShow("Admin:", "Deleted");
  } else {
    Serial.printf("❌ Sensor deletion failed (Error code: %d)\n", result);
    lcdShow("Admin:", "Delete Failed");
  }
  delay(800);
}

void deleteAllFingerprints() {
  if (enrolledCount == 0) {
    Serial.println("⚠ No fingerprints stored.");
    lcdShow("Admin:", "No Students");
    return;
  }

  Serial.println("\n⚠ Are you sure you want to delete ALL fingerprints? (y/n)");
  lcdShow("Admin:", "Confirm y/n");
  while (Serial.available() == 0);
  char confirm = Serial.read();
  while (Serial.available()) Serial.read();

  if (confirm != 'y' && confirm != 'Y') {
    Serial.println("❌ Deletion cancelled.");
    lcdShow("Admin:", "Cancelled");
    return;
  }

  Serial.println("🧹 Deleting all fingerprints from sensor, Google Sheet, and local memory...");
  lcdShow("Admin:", "Deleting All");
  delay(800);

  uint8_t result = finger.emptyDatabase();
  if (result == FINGERPRINT_OK) {
    Serial.println("✅ All fingerprints deleted from sensor.");
  } else {
    Serial.printf("❌ Sensor deletion failed (Error code: %d)\n", result);
    Serial.println("⚠ Aborting Google Sheet and memory reset to avoid mismatch.");
    lcdShow("Admin:", "Sensor Error");
    return;
  }

  updateGoogleSheet("clearAll", 0, "ALL");
  delay(800);

  for (int i = 0; i < 127; i++) {
    fingerList[i].id = 0;
    fingerList[i].name = "";
    fingerList[i].busNo = "";
    fingerList[i].parent = "";
    fingerList[i].parentChatId = "";
  }
  enrolledCount = 0;

  Serial.println("✅ All data cleared (sensor, sheet, local cache).");
  lcdShow("Admin:", "All Deleted");
}

void listFingerprints() {
  Serial.println("\n🧾 Enrolled Fingerprints:");
  lcdShow("Admin:", "Listing...");
  bool any = false;
  for (int i = 0; i < 127; i++) {
    if (fingerList[i].id != 0) {
      String name   = fingerList[i].name;
      String busNo  = fingerList[i].busNo;
      String parent = fingerList[i].parent;
      String chatId = fingerList[i].parentChatId;

      Serial.print("🆔 ID: ");   Serial.print(fingerList[i].id);
      Serial.print(" | Name: "); Serial.print(name);
      Serial.print(" | Bus: ");  Serial.print(busNo);
      Serial.print(" | Parent: "); Serial.print(parent);
      Serial.print(" | Parent chat id: "); Serial.println(chatId);

      any = true;
      delay(20);
    }
  }
  if (!any) {
    Serial.println("⚠  No fingerprints enrolled.");
    lcdShow("Admin:", "No Students");
  } else {
    lcdShow("Admin:", "List Done");
  }
  delay(800);
}

void runAdminMode() {
  Serial.println("🔵 Admin mode (Step 1) selected");
  lcdShow("Mode:", "Admin");

  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi connection failed. Continuing offline...");
    lcdShow("Admin:", "WiFi Offline");
  }

  if (finger.verifyPassword()) {
    Serial.println("✅ Fingerprint sensor detected successfully!");
  } else {
    Serial.println("❌ Sensor not found! Check wiring and try again.");
    lcdShow("Admin:", "Sensor Error");
    while (true) delay(1000);
  }

  syncFromGoogleSheet();
  delay(800);
  lcdShow("Admin:", "Ready");
  showMenu();

  while (true) {
    ensureWiFiConnected();
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.length() == 0) continue;
      char option = input.charAt(0);

      Serial.println();
      switch (option) {
        case '1': enrollFingerprint();       break;
        case '2': verifyFingerprintAdmin();  break;
        case '3': deleteFingerprintByID();   break;
        case '4': deleteAllFingerprints();   break;
        case '5': listFingerprints();        break;
        default:  Serial.println("❌ Invalid option! Please select 1-5."); lcdShow("Admin:", "Invalid Opt"); break;
      }
      delay(800);
      lcdShow("Admin:", "Ready");
      showMenu();
    }
  }
}

// ========== STEP 3: TRANSPORT MODE (Bus + Telegram) ==========

const int MAX_STUDENTS = 128;
unsigned long lastScanTime[MAX_STUDENTS];
uint16_t scanCount[MAX_STUDENTS];
const unsigned long studentCooldown = 300000; // 5 minutes

struct StudentInfo {
  bool   found;
  String name;
  String busNo;
  String parentChatId;
};

// explicit prototype
StudentInfo getStudentInfoFromSheet(int studentId);

int getFingerprintIDWithBuffer() {
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_NOFINGER) return -1;
  if (p != FINGERPRINT_OK)      return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK)      return -1;

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK)      return finger.fingerID;
  if (p == FINGERPRINT_NOTFOUND) return -2;

  return -1;
}

StudentInfo getStudentInfoFromSheet(int studentId) {
  StudentInfo info;
  info.found = false;

  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected! Cannot query Google Sheets.");
    lcdShow("Transport:", "WiFi Error");
    return info;
  }

  HTTPClient http;
  String url = String(GOOGLE_SCRIPT_URL) + "?action=readOne&id=" + String(studentId);
  Serial.print("🌐 Querying student info: ");
  Serial.println(url);

  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("✅ Student info payload:");
    Serial.println(payload);

    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc.is<JsonObject>()) {
      int id = doc["ID"] | 0;
      if (id == studentId) {
        info.name         = doc["Name"].as<String>();
        info.busNo        = doc["BusNo"].as<String>();
        info.parentChatId = doc["ParentChatId"].as<String>();
        if (info.parentChatId.length() > 0) info.found = true;
      }
    } else {
      Serial.print("❌ JSON parse error: ");
      Serial.println(err.c_str());
    }
  } else {
    Serial.print("❌ HTTP error from sheet: ");
    Serial.println(httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return info;
}

void sendTelegramMessage(const String& chatId, String message) {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected! Cannot send Telegram message.");
    lcdShow("Telegram:", "WiFi Error");
    return;
  }
  if (String(TELEGRAM_BOT_TOKEN).length() == 0 || chatId.length() == 0) {
    Serial.println("⚠️ Telegram BOT token or chatId not set!");
    lcdShow("Telegram:", "Config Error");
    return;
  }

  HTTPClient http;
  String url = "https://api.telegram.org/bot" +
               String(TELEGRAM_BOT_TOKEN) + "/sendMessage";

  message.replace("\"", "'");

  String payload = "{\"chat_id\":\"" + chatId +
                   "\",\"text\":\"" + message + "\"}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.print("✅ Telegram HTTP response code: ");
    Serial.println(httpCode);
  } else {
    Serial.print("❌ Error on sending Telegram POST: ");
    Serial.println(httpCode);
  }
  http.end();
}

void sendTelegramLocation(const String& chatId, double lat, double lng) {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected! Cannot send Telegram location.");
    lcdShow("Telegram:", "WiFi Error");
    return;
  }
  if (String(TELEGRAM_BOT_TOKEN).length() == 0 || chatId.length() == 0) {
    Serial.println("⚠️ Telegram BOT token or chatId not set!");
    lcdShow("Telegram:", "Config Error");
    return;
  }

  HTTPClient http;
  String url = "https://api.telegram.org/bot" +
               String(TELEGRAM_BOT_TOKEN) + "/sendLocation";

  String payload = "{\"chat_id\":\"" + chatId +
                   "\",\"latitude\":" + String(lat, 6) +
                   ",\"longitude\":" + String(lng, 6) + "}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.print("✅ Telegram location HTTP response code: ");
    Serial.println(httpCode);
  } else {
    Serial.print("❌ Error on sending Telegram location POST: ");
    Serial.println(httpCode);
  }
  http.end();
}

void buzzForVerified(unsigned long ms) {
  tone(BUZZER_PIN, 2000);
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    while (gpsSerial.available() > 0) gps.encode(gpsSerial.read());
    delay(10);
  }
  noTone(BUZZER_PIN);
}

void getAccurateGPSLocationOnce(bool isBoarding, int fingerprintID) {
  StudentInfo info = getStudentInfoFromSheet(fingerprintID);
  if (!info.found) {
    Serial.println("⚠️ Could not find student or ParentChatId in sheet. Will not send to parent.");
    lcdShow("Transport:", "Student Missing");
  } else {
    lcdShow("Transport:", "Student Found");
  }

  unsigned long stabilizeStart = millis();
  while (millis() - stabilizeStart < 1000) {
    while (gpsSerial.available() > 0) gps.encode(gpsSerial.read());
  }

  unsigned long start = millis();
  bool locationFound = false;
  double lat = 0, lng = 0;

  while (millis() - start < 10000) {
    while (gpsSerial.available() > 0) {
      if (gps.encode(gpsSerial.read())) {
        if (gps.location.isValid()) {
          lat = gps.location.lat();
          lng = gps.location.lng();
          locationFound = true;
          break;
        }
      }
    }
    if (locationFound) break;
    delay(50);
  }

  String timeStr = getTimestampIST();
  String eventMsg = isBoarding ? "boarded" : "deboarded";

  String who, bus;
  if (info.found) {
    who = "Student \"" + info.name + "\" (ID " + String(fingerprintID) + ")";
    bus = " bus " + info.busNo;
  } else {
    who = "Student ID " + String(fingerprintID);
    bus = " the bus";
  }

  if (locationFound) {
    String googleMapsLink = "https://maps.google.com/?q=" +
                            String(lat, 6) + "," + String(lng, 6);
    String finalMessage = who + " has " + eventMsg + bus +
                          " at " + timeStr +
                          ". Location: " + googleMapsLink;

    if (info.found) {
      sendTelegramMessage(info.parentChatId, finalMessage);
      sendTelegramLocation(info.parentChatId, lat, lng);
      Serial.println("✅ GPS found. Message and location sent to parent.");
      lcdShow("Attendance", "Marked");
      delay(1200);
      lcdShow("Telegram", "Message Sent");
    } else {
      Serial.println("⚠️ ParentChatId not available, message not sent.");
      lcdShow("Transport:", "No Chat ID");
    }
  } else {
    String finalMessage = who + " has " + eventMsg + bus +
                          " at " + timeStr +
                          ". Location: Not available";

    if (info.found) {
      sendTelegramMessage(info.parentChatId, finalMessage);
      Serial.println("⚠️ GPS not found. Message sent without location.");
      lcdShow("Attendance", "Marked");
      delay(1200);
      lcdShow("Telegram", "Message Sent");
    } else {
      Serial.println("⚠️ ParentChatId not available, message not sent.");
      lcdShow("Transport:", "Student Missing");
    }
  }

  delay(1500);
  lcdShow("Transport", "Ready for Attd");
}

void runTransportMode() {
  Serial.println("🟢 Transport mode (Step 3) selected");
  lcdShow("Mode:", "Transport");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  ensureWiFiConnected();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    delay(500);
    lcdShow("Transport:", "WiFi OK");
  } else {
    Serial.println("❌ Failed to connect to WiFi. Time via NTP will be unavailable.");
    lcdShow("Transport:", "WiFi Failed");
  }

  fpSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  delay(500);
  finger.begin(57600);
  delay(500);
  if (finger.verifyPassword()) {
    Serial.println("✅ Fingerprint sensor detected.");
  } else {
    Serial.println("❌ Fingerprint sensor not detected! Check wiring (TX/RX pins).");
    lcdShow("Transport:", "Sensor Error");
    while (1) delay(1000);
  }

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("✅ GPS module initialized.");
  Serial.println("Place your finger on the sensor...");
  lcdShow("Transport", "Ready for Attd");

  for (int i = 0; i < MAX_STUDENTS; i++) {
    lastScanTime[i] = 0;
    scanCount[i] = 0;
  }

  while (true) {
    ensureWiFiConnected();

    int initialResult = getFingerprintIDWithBuffer();
    if (initialResult == -1) {
      delay(150);
      continue;
    }

    if (initialResult == -2) {
      int failCount = 1;
      int fingerprintID = -1;

      while (failCount < 3) {
        Serial.println();
        Serial.print("❗ Try again (");
        Serial.print(failCount + 1);
        Serial.println("/3)");

        lcdShow("Transport", "Scan Again");

        unsigned long t0 = millis();
        int scanResult = -1;
        while (millis() - t0 < 7000) {
          scanResult = getFingerprintIDWithBuffer();
          if (scanResult != -1) break;
          delay(100);
        }

        if (scanResult > 0) {
          fingerprintID = scanResult;
          break;
        } else if (scanResult == -2) {
          failCount++;
          continue;
        } else {
          failCount++;
          continue;
        }
      }

      if (fingerprintID <= 0) {
        Serial.println();
        Serial.println("❌ Not registered");
        Serial.println();
        lcdShow("Transport", "Not Registered");
        delay(1500);
        lcdShow("Transport", "Ready for Attd");
        continue;
      }
      initialResult = fingerprintID;
    }

    if (initialResult > 0) {
      int fingerprintID = initialResult;

      if (fingerprintID <= 0 || fingerprintID >= MAX_STUDENTS) {
        Serial.print("⚠️ Finger ID out of range: ");
        Serial.println(fingerprintID);
        lcdShow("Transport", "ID Out Range");
        continue;
      }

      unsigned long now = millis();
      if (scanCount[fingerprintID] > 0 &&
          (now - lastScanTime[fingerprintID] < studentCooldown)) {
        unsigned long remaining =
          (studentCooldown - (now - lastScanTime[fingerprintID])) / 1000;
        Serial.println("⏳ This student has already scanned recently.");
        Serial.print("   Please wait ");
        Serial.print(remaining);
        Serial.println(" more seconds.");
        lcdShow("Transport", "Wait " + String(remaining) + "s");
        continue;
      }

      lastScanTime[fingerprintID] = now;
      scanCount[fingerprintID]++;

      bool isBoarding = (scanCount[fingerprintID] % 2 == 1);

      Serial.println();
      Serial.println("✅ Fingerprint verified!");
      Serial.print("👤 Finger ID (Student): ");
      Serial.println(fingerprintID);
      Serial.println(isBoarding ? "🎒 Event: BOARDING" : "🏠 Event: DEBOARDING");

      lcdShow("Attendance", "Processing...");
      buzzForVerified(3000);

      Serial.println("📡 Fetching location and student info...");
      getAccurateGPSLocationOnce(isBoarding, fingerprintID);

      Serial.println("Waiting for next student...");
      Serial.println();
    }
  }
}

// =================== GLOBAL SETUP / LOOP ===================

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdShow("EduRoute System", "Initializing...");

  fpSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  pinMode(MODE_PIN, INPUT_PULLUP);

  bool modePinState = digitalRead(MODE_PIN);
  Serial.println("\n==========================================");
  Serial.println("   EduRoute Combined Firmware");
  Serial.println("==========================================");
  Serial.print("MODE_PIN state: ");
  Serial.println(modePinState == HIGH ? "HIGH" : "LOW");

  WiFi.mode(WIFI_STA);

  if (modePinState == LOW) {
    // LOW -> button pressed -> Transport mode
    runTransportMode();
  } else {
    // HIGH -> button not pressed -> Admin mode
    runAdminMode();
  }
}

void loop() {
  // not used; both modes have their own while(true) loops
}
