/*
  WT32-ETH01 NFC + Relay Controller
  - Ethernet primary, WiFi fallback (STA or AP)
  - UID list stored in EEPROM (UID;relayMask)
  - Web UI: view/add/remove records, WiFi config
  - Add last scanned UID via hardware button
  - PN532 via I2C (Wire)
*/

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <time.h>

// -------------------- Піни --------------------
#define I2C_SDA 33
#define I2C_SCL 32
#define PN532_IRQ 4
#define PN532_RESET 5
#define RELAY_1 14
#define RELAY_2 15
#define BUTTON_ADD 39   // GPIO39 не має внутрішньої підтяжки: потрібен зовнішній резистор 10 кОм до 3.3 V

// -------------------- EEPROM --------------------
#define EEPROM_SIZE 2048
#define ADDR_LIST_START 0
#define SLOT_MAX 32           // макс байтів на запис (UID + ";" + relayChar + '\0')
#define LEGACY_MAX_SLOTS 30
#define MAX_CARDS 300
#define CARD_FILE "/cards.jsonl"
#define CARD_TEMP_FILE "/cards.tmp"
#define CARD_BACKUP_FILE "/cards.bak"
#define ATTENDANCE_FILE "/attendance.jsonl"
#define ATTENDANCE_TEMP_FILE "/attendance.tmp"
#define ATTENDANCE_BACKUP_FILE "/attendance.bak"
#define MAX_ATTENDANCE_EVENTS_PER_TYPE 10
#define RECENT_SCAN_LIMIT 10
#define RECENT_UID_FIELD_SIZE 21
#define READ_TIME_EPOCH_BASE 1704067200UL
#define UID_FIELD_SIZE 24
#define NAME_FIELD_SIZE 64
#define VEHICLE_FIELD_SIZE 24
#define BIRTH_DATE_FIELD_SIZE 11
#define ADDR_WIFI_SSID 1000
#define ADDR_WIFI_PASS 1040
#define WIFI_MAX_SSID 33 // 32 символи SSID + завершальний нуль
#define WIFI_MAX_PASS 64
#define ADDR_CLICK_DURATION 1104
#define ADDR_MQTT_HOST 1120
#define MQTT_MAX_HOST 65
#define ADDR_MQTT_PORT 1186
#define ADDR_MQTT_USER 1190
#define MQTT_MAX_USER 33
#define ADDR_MQTT_PASS 1224
#define MQTT_MAX_PASS 65
#define ADDR_MQTT_TOPIC 1290
#define MQTT_MAX_TOPIC 65
#define ADDR_CARD_FS_MARKER 1400
#define ADDR_CARD_LITTLEFS_MARKER 1401

struct __attribute__((packed)) CardRecord {
  char uid[UID_FIELD_SIZE];
  char lastName[NAME_FIELD_SIZE];
  char firstName[NAME_FIELD_SIZE];
  char middleName[NAME_FIELD_SIZE];
  char vehicle[VEHICLE_FIELD_SIZE];
  char birthDate[BIRTH_DATE_FIELD_SIZE];
  uint8_t relayMask;
  uint16_t readCount;
  uint8_t lastReadMinutes[3];
};

struct __attribute__((packed)) AttendanceEvent {
  char uid[RECENT_UID_FIELD_SIZE];
  uint8_t epochMinutes[3];
  bool arrived;
};

CardRecord *cardRecords = nullptr;
size_t cardCount = 0;
bool cardStorageReady = false;
AttendanceEvent attendanceEvents[MAX_ATTENDANCE_EVENTS_PER_TYPE * 2];
uint8_t attendanceEventCount = 0;
char recentScanUIDs[RECENT_SCAN_LIMIT][RECENT_UID_FIELD_SIZE];
size_t recentScanCount = 0;
bool cardStatsDirty = false;
unsigned long lastCardStatsSave = 0;

bool isValidUIDFormat(const String &uid);
String normalizeUIDInput(String in);
bool addRecord(const String &uid, uint8_t relayMask);
bool addRecord(const String &uid, uint8_t relayMask, const String &lastName,
               const String &firstName, const String &middleName,
               const String &vehicle, const String &birthDate);
bool updateCardRecord(size_t index, uint8_t relayMask, const String &lastName,
                      const String &firstName, const String &middleName,
                      const String &vehicle, const String &birthDate);
bool removeUIDRecord(const String &uid);
uint8_t findUIDRelayMask(const String &uid);
String getListJSON();
bool initializeAttendanceStorage();
bool saveAttendanceEvents();
void trackCardRead(const String &uid);
void seedRecentScans();
String getActivityJSON();

// -------------------- PN532 --------------------
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET, &Wire);

// -------------------- Веб/Мережа --------------------
AsyncWebServer asyncServer(80);
AsyncWebSocket webSocket("/ws");
fs::LittleFSFS cardFS;
class AsyncServerFacade {
public:
  AsyncWebServerRequest *request = nullptr;
  JsonDocument jsonBody;

  void begin() { asyncServer.begin(); }
  void handleClient() {}
  void on(const char *uri, WebRequestMethod method, void (*handler)()) {
    asyncServer.on(uri, method,
      [this, handler](AsyncWebServerRequest *incoming) {
        request = incoming;
        jsonBody.clear();
        if (incoming->_tempObject) {
          String *body = static_cast<String *>(incoming->_tempObject);
          if (body->length() > 0) deserializeJson(jsonBody, *body);
          delete body;
          incoming->_tempObject = nullptr;
        }
        handler();
        request = nullptr;
      }, nullptr,
      [](AsyncWebServerRequest *incoming, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (index == 0) incoming->_tempObject = new String();
        String *body = static_cast<String *>(incoming->_tempObject);
        if (body && index + len <= total) body->concat((const char *)data, len);
      });
  }
  void onNotFound(void (*handler)()) {
    asyncServer.onNotFound([this, handler](AsyncWebServerRequest *incoming) {
      request = incoming;
      handler();
      request = nullptr;
    });
  }
  bool authenticate(const char *user, const char *pass) {
    return request && request->authenticate(user, pass);
  }
  void requestAuthentication() {
    if (request) request->requestAuthentication();
  }
  String arg(const char *name) {
    if (!request) return String();
    if (request->hasParam(name, true)) return request->getParam(name, true)->value();
    if (request->hasParam(name)) return request->getParam(name)->value();
    JsonVariantConst value = jsonBody[name];
    if (value.is<const char *>()) return String(value.as<const char *>());
    if (value.is<long>()) return String(value.as<long>());
    if (value.is<double>()) return String(value.as<double>());
    if (value.is<bool>()) return value.as<bool>() ? "true" : "false";
    return String();
  }
  bool hasArg(const char *name) {
    return request && (request->hasParam(name, true) || request->hasParam(name) ||
                       !jsonBody[name].isNull());
  }
  WebRequestMethod method() {
    return request ? request->method() : HTTP_GET;
  }
  void sendHeader(const char *, const char *, bool = false) {}
  void send(int statusCode, const char *contentType, const String &body) {
    if (request) request->send(statusCode, contentType, body);
  }
};
AsyncServerFacade server;
DNSServer dnsServer; 
WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
String mqttServerHost;
bool apActive = false;
bool captiveDnsActive = false;
const char* webUser = "admin";
const char* webPass = "ChangeMe123!"; // Змініть на свій пароль

// ETH параметри (WT32-ETH01)
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN
#define ETH_POWER_PIN   16
#define ETH_TYPE        ETH_PHY_LAN8720
#define ETH_ADDR        1
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18

// -------------------- Змінні --------------------
String lastScannedUID = "";
bool nfcPresent = false;
int clickDuration = 1000;

// Таймери реле
unsigned long relay1_turnOffTime = 0;
bool relay1_active = false;
unsigned long relay2_turnOffTime = 0;
bool relay2_active = false;

// NFC debounce
unsigned long lastNFCRead = 0;
const unsigned long NFC_POLL_INTERVAL = 400; // ms
const unsigned long CARD_REARM_MS = 800;
String presentedUID = "";
unsigned long lastCardSeen = 0;
unsigned long lastNfcInitAttempt = 0;
unsigned long lastNfcFailureLog = 0;
unsigned long lastNetworkCheck = 0;
const unsigned long NFC_RETRY_INTERVAL = 5000;
const unsigned long CARD_STATS_SAVE_INTERVAL = 60000;
unsigned long lastMqttAttempt = 0;
unsigned long lastMqttStatusPublish = 0;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
const unsigned long MQTT_STATUS_INTERVAL = 30000;
bool lastPublishedNfcPresent = false;
bool wifiConnectPending = false;
unsigned long wifiConnectPendingAt = 0;
bool wifiScanInProgress = false;
String wifiScanResultJson;

// Мережевий режим
enum NetMode { NET_NONE, NET_ETH, NET_WIFI_STA, NET_WIFI_AP };
NetMode currentNet = NET_NONE;
NetMode lastPublishedNet = NET_NONE;
NetMode timeSyncMode = NET_NONE;
unsigned long lastWebSocketBroadcast = 0;
enum NetworkStartup { NET_START_ETH, NET_START_WIFI, NET_START_DONE };
NetworkStartup networkStartup = NET_START_ETH;
unsigned long networkPhaseStarted = 0;

void ensureTimeSync() {
  if (currentNet != NET_ETH && currentNet != NET_WIFI_STA) {
    timeSyncMode = NET_NONE;
    return;
  }
  if (timeSyncMode == currentNet) return;
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  timeSyncMode = currentNet;
  Serial.println("Запущено синхронізацію часу NTP для часового поясу Europe/Kyiv.");
}

String buildLiveStatusJSON(const char *type) {
  JsonDocument doc;
  doc["type"] = type;
  doc["uid"] = lastScannedUID;
  doc["pn532"] = nfcPresent;
  doc["network"] = (currentNet == NET_ETH) ? "Ethernet" :
      (currentNet == NET_WIFI_STA) ? "WiFi" :
      (currentNet == NET_WIFI_AP) ? "AP" : "offline";
  doc["ip"] = (currentNet == NET_ETH) ? ETH.localIP().toString() :
      (currentNet == NET_WIFI_STA) ? WiFi.localIP().toString() :
      (currentNet == NET_WIFI_AP) ? WiFi.softAPIP().toString() : "";
  time_t now;
  time(&now);
  doc["epoch"] = now >= 1704067200 ? (uint32_t)now : 0;
  String json;
  serializeJson(doc, json);
  return json;
}

uint32_t attendanceEventEpoch(const AttendanceEvent &event) {
  uint32_t storedMinutes = (uint32_t)event.epochMinutes[0] |
      ((uint32_t)event.epochMinutes[1] << 8) |
      ((uint32_t)event.epochMinutes[2] << 16);
  if (storedMinutes == 0) return 0;
  return READ_TIME_EPOCH_BASE + (storedMinutes - 1) * 60;
}

void setAttendanceEventEpoch(AttendanceEvent &event, uint32_t epoch) {
  uint32_t storedMinutes = epoch < READ_TIME_EPOCH_BASE
      ? 0 : (epoch - READ_TIME_EPOCH_BASE) / 60 + 1;
  if (storedMinutes > 0xFFFFFFUL) storedMinutes = 0xFFFFFFUL;
  event.epochMinutes[0] = (uint8_t)(storedMinutes & 0xFF);
  event.epochMinutes[1] = (uint8_t)((storedMinutes >> 8) & 0xFF);
  event.epochMinutes[2] = (uint8_t)((storedMinutes >> 16) & 0xFF);
}

// -------------------- EEPROM допоміжні --------------------
String eepromReadString(int offset, int maxLen) {
  if (maxLen <= 0) return String();
  char buf[maxLen + 1];
  for (int i = 0; i < maxLen; i++) {
    uint8_t b = EEPROM.read(offset + i);
    if (b == 0xFF || b == 0x00) { buf[i] = '\0'; return String(buf); }
    buf[i] = (char)b;
  }
  buf[maxLen] = '\0';
  return String(buf);
}

void eepromWriteString(int offset, const String &s, int maxLen) {
  int len = min((int)s.length(), maxLen - 1);
  for (int i = 0; i < len; i++) EEPROM.write(offset + i, (uint8_t)s[i]);
  EEPROM.write(offset + len, 0);
  for (int i = len + 1; i < maxLen; i++) EEPROM.write(offset + i, 0);
}

// Слоти для списку
void eepromWriteSlot(int slotIndex, const String &s) {
  if (slotIndex < 0 || slotIndex >= LEGACY_MAX_SLOTS) return;
  int base = ADDR_LIST_START + slotIndex * SLOT_MAX;
  eepromWriteString(base, s, SLOT_MAX);
}

String eepromReadSlot(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= LEGACY_MAX_SLOTS) return String();
  int base = ADDR_LIST_START + slotIndex * SLOT_MAX;
  return eepromReadString(base, SLOT_MAX);
}

// -------------------- UID форматування --------------------
String formatUID(const uint8_t *uid, uint8_t uidLength) {
  String s = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
    if (i < uidLength - 1) s += " ";
  }
  s.toUpperCase();
  return s;
}

String normalizeUIDInput(String in) {
  in.trim();
  in.toUpperCase();
  String out = "";
  bool lastWasSpace = false;
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ' ' || c == '\t') {
      if (!lastWasSpace) { out += ' '; lastWasSpace = true; }
    } else {
      out += c; lastWasSpace = false;
    }
  }
  out.trim();
  return out;
}

bool isValidUIDFormat(const String &uid) {
  if (uid.length() == 0) return false;
  int tokenLen = 0;
  int tokens = 0;
  for (unsigned int i = 0; i < uid.length(); i++) {
    char c = uid[i];
    if (c == ' ') {
      if (tokenLen == 0) return false;
      if (tokenLen != 2) return false;
      tokenLen = 0;
      tokens++;
    } else {
      if (!isxdigit(c)) return false;
      tokenLen++;
      if (tokenLen > 2) return false;
    }
  }
  if (tokenLen == 0) return false;
  if (tokenLen != 2) return false;
  tokens++;
  return tokens >= 1 && tokens <= 7;
}

bool isValidBirthDate(const String &date) {
  if (date.length() == 0) return true;
  if (date.length() != 10 || date[4] != '-' || date[7] != '-') return false;
  for (unsigned int i = 0; i < date.length(); i++) {
    if (i != 4 && i != 7 && !isDigit(date[i])) return false;
  }
  int year = date.substring(0, 4).toInt();
  int month = date.substring(5, 7).toInt();
  int day = date.substring(8, 10).toInt();
  if (year < 1 || month < 1 || month > 12) return false;
  const uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDay = daysPerMonth[month - 1];
  bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (month == 2 && leapYear) maxDay = 29;
  return day >= 1 && day <= maxDay;
}

void copyCardField(char *destination, size_t capacity, const String &value) {
  if (capacity == 0) return;
  size_t length = min(value.length(), capacity - 1);
  memcpy(destination, value.c_str(), length);
  destination[length] = '\0';
}

uint32_t cardLastReadEpoch(const CardRecord &record) {
  uint32_t storedMinutes = (uint32_t)record.lastReadMinutes[0] |
      ((uint32_t)record.lastReadMinutes[1] << 8) |
      ((uint32_t)record.lastReadMinutes[2] << 16);
  if (storedMinutes == 0) return 0;
  return READ_TIME_EPOCH_BASE + (storedMinutes - 1) * 60;
}

void setCardLastReadEpoch(CardRecord &record, uint32_t epoch) {
  if (epoch < READ_TIME_EPOCH_BASE) {
    memset(record.lastReadMinutes, 0, sizeof(record.lastReadMinutes));
    return;
  }
  uint32_t storedMinutes = (epoch - READ_TIME_EPOCH_BASE) / 60 + 1;
  if (storedMinutes > 0xFFFFFFUL) storedMinutes = 0xFFFFFFUL;
  record.lastReadMinutes[0] = (uint8_t)(storedMinutes & 0xFF);
  record.lastReadMinutes[1] = (uint8_t)((storedMinutes >> 8) & 0xFF);
  record.lastReadMinutes[2] = (uint8_t)((storedMinutes >> 16) & 0xFF);
}

bool cardUIDExists(const String &uid) {
  for (size_t i = 0; i < cardCount; i++) {
    if (uid == cardRecords[i].uid) return true;
  }
  return false;
}

bool loadCardFile(const char *path) {
  File file = cardFS.open(path, FILE_READ);
  if (!file) return false;
  cardCount = 0;
  size_t invalidRows = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() == 0) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line)) {
      invalidRows++;
      continue;
    }

    String uid = normalizeUIDInput(String(doc["uid"] | ""));
    int relay = doc["relay"] | 1;
    if (!isValidUIDFormat(uid) || relay < 1 || relay > 3 ||
        cardCount >= MAX_CARDS || cardUIDExists(uid)) {
      invalidRows++;
      continue;
    }

    CardRecord &record = cardRecords[cardCount++];
    memset(&record, 0, sizeof(record));
    copyCardField(record.uid, sizeof(record.uid), uid);
    copyCardField(record.lastName, sizeof(record.lastName), String(doc["lastName"] | ""));
    copyCardField(record.firstName, sizeof(record.firstName), String(doc["firstName"] | ""));
    copyCardField(record.middleName, sizeof(record.middleName), String(doc["middleName"] | ""));
    copyCardField(record.vehicle, sizeof(record.vehicle), String(doc["vehicle"] | ""));
    copyCardField(record.birthDate, sizeof(record.birthDate), String(doc["birthDate"] | ""));
    record.relayMask = (uint8_t)relay;
    record.readCount = doc["readCount"] | 0;
    setCardLastReadEpoch(record, doc["lastReadEpoch"] | 0UL);
  }
  file.close();
  if (invalidRows > 0) {
    Serial.print("Некоректних або повторних рядків у файлі карток: ");
    Serial.println(invalidRows);
    return false;
  }
  return true;
}

bool saveCardRecords() {
  cardFS.remove(CARD_TEMP_FILE);
  File file = cardFS.open(CARD_TEMP_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("SPIFFS: не вдалося відкрити тимчасовий файл карток для запису.");
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < cardCount && ok; i++) {
    JsonDocument doc;
    JsonObject item = doc.to<JsonObject>();
    item["uid"] = cardRecords[i].uid;
    item["relay"] = cardRecords[i].relayMask;
    item["lastName"] = cardRecords[i].lastName;
    item["firstName"] = cardRecords[i].firstName;
    item["middleName"] = cardRecords[i].middleName;
    item["vehicle"] = cardRecords[i].vehicle;
    item["birthDate"] = cardRecords[i].birthDate;
    item["readCount"] = cardRecords[i].readCount;
    item["lastReadEpoch"] = cardLastReadEpoch(cardRecords[i]);
    ok = serializeJson(doc, file) > 0 && file.write('\n') == 1;
  }
  file.close();
  if (!ok) {
    cardFS.remove(CARD_TEMP_FILE);
    Serial.println("SPIFFS: помилка під час запису тимчасового файлу карток.");
    return false;
  }

  cardFS.remove(CARD_BACKUP_FILE);
  bool hadCurrent = cardFS.exists(CARD_FILE);
  if (hadCurrent && !cardFS.rename(CARD_FILE, CARD_BACKUP_FILE)) {
    cardFS.remove(CARD_TEMP_FILE);
    Serial.println("SPIFFS: не вдалося створити резервну копію поточного списку.");
    return false;
  }
  if (!cardFS.rename(CARD_TEMP_FILE, CARD_FILE)) {
    if (hadCurrent) cardFS.rename(CARD_BACKUP_FILE, CARD_FILE);
    Serial.println("SPIFFS: не вдалося замінити файл списку карток.");
    return false;
  }
  if (hadCurrent) cardFS.remove(CARD_BACKUP_FILE);
  return true;
}

bool initializeCardStorage() {
  bool firstMount = EEPROM.read(ADDR_CARD_FS_MARKER) != 0xA5;
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("Помилка монтування LittleFS для вебфайлів.");
    return false;
  }
  bool firstCardFSMount = EEPROM.read(ADDR_CARD_LITTLEFS_MARKER) != 0x5A;
  if (!cardFS.begin(firstCardFSMount, "/cardfs", 10, "cardfs")) {
    Serial.println("Помилка монтування LittleFS зі сховищем карток.");
    return false;
  }
  if (!SPIFFS.begin(false, "/legacy", 10, "legacy")) {
    Serial.println("Старий розділ SPIFFS недоступний; міграція файлів пропущена.");
  } else {
    const char *legacyFiles[] = {CARD_FILE, ATTENDANCE_FILE};
    for (const char *path : legacyFiles) {
      if (cardFS.exists(path) || !SPIFFS.exists(path)) continue;
      File source = SPIFFS.open(path, FILE_READ);
      File target = cardFS.open(path, FILE_WRITE);
      if (!source || !target) {
        Serial.print("Не вдалося відкрити файл для міграції з SPIFFS: ");
        Serial.println(path);
        if (source) source.close();
        if (target) target.close();
        return false;
      }
      uint8_t buffer[256];
      while (source.available()) {
        size_t count = source.read(buffer, sizeof(buffer));
        if (count == 0 || target.write(buffer, count) != count) {
          source.close();
          target.close();
          Serial.print("Помилка копіювання файла SPIFFS до LittleFS: ");
          Serial.println(path);
          return false;
        }
      }
      source.close();
      target.close();
      Serial.print("Дані перенесено зі SPIFFS до LittleFS: ");
      Serial.println(path);
    }
  }
  if (firstCardFSMount) {
    EEPROM.write(ADDR_CARD_LITTLEFS_MARKER, 0x5A);
    if (!EEPROM.commit()) {
      Serial.println("Не вдалося позначити ініціалізацію сховища LittleFS карток.");
      return false;
    }
  }
  if (firstMount) {
    EEPROM.write(ADDR_CARD_FS_MARKER, 0xA5);
    if (!EEPROM.commit()) {
      Serial.println("Не вдалося позначити ініціалізацію сховища карток.");
      return false;
    }
  }

  if (!cardFS.exists(CARD_FILE)) {
    if (cardFS.exists(CARD_BACKUP_FILE)) {
      if (!cardFS.rename(CARD_BACKUP_FILE, CARD_FILE)) {
        Serial.println("Не вдалося відновити резервний файл карток.");
        return false;
      }
    } else if (cardFS.exists(CARD_TEMP_FILE)) {
      if (!cardFS.rename(CARD_TEMP_FILE, CARD_FILE)) {
        Serial.println("Не вдалося відновити тимчасовий файл карток.");
        return false;
      }
    } else {
      cardCount = 0;
      for (int i = 0; i < LEGACY_MAX_SLOTS; i++) {
        String legacy = eepromReadSlot(i);
        if (legacy.length() == 0) break;
        int separator = legacy.indexOf(';');
        if (separator <= 0) continue;

        String uid = normalizeUIDInput(legacy.substring(0, separator));
        int relay = legacy.substring(separator + 1).toInt();
        if (!isValidUIDFormat(uid) || relay < 1 || relay > 3 ||
            cardCount >= MAX_CARDS || cardUIDExists(uid)) continue;

        CardRecord &record = cardRecords[cardCount++];
        memset(&record, 0, sizeof(record));
        copyCardField(record.uid, sizeof(record.uid), uid);
        record.relayMask = (uint8_t)relay;
      }
      if (!saveCardRecords()) {
        Serial.println("Не вдалося перенести старий список UID у SPIFFS.");
        return false;
      }
      Serial.print("Список карток створено; перенесено старих записів: ");
      Serial.println(cardCount);
      return true;
    }
  }

  if (!loadCardFile(CARD_FILE)) {
    if (!cardFS.exists(CARD_BACKUP_FILE) || !loadCardFile(CARD_BACKUP_FILE)) {
      Serial.println("Не вдалося прочитати файл списку карток або його резервну копію.");
      return false;
    }
    cardFS.remove(CARD_FILE);
    if (!cardFS.rename(CARD_BACKUP_FILE, CARD_FILE)) {
      Serial.println("Не вдалося відновити резервну копію списку карток.");
      return false;
    }
    Serial.println("Список карток відновлено з резервної копії.");
  }
  Serial.print("Список карток завантажено: ");
  Serial.println(cardCount);
  return true;
}

bool addRecord(const String &uid, uint8_t relayMask) {
  return addRecord(uid, relayMask, "", "", "", "", "");
}

bool addRecord(const String &uid, uint8_t relayMask, const String &lastName,
               const String &firstName, const String &middleName,
               const String &vehicle, const String &birthDate) {
  String normalizedUID = normalizeUIDInput(uid);
  if (!cardStorageReady || !isValidUIDFormat(normalizedUID) ||
      relayMask < 1 || relayMask > 3 || cardUIDExists(normalizedUID) ||
      cardCount >= MAX_CARDS || lastName.length() >= NAME_FIELD_SIZE ||
      firstName.length() >= NAME_FIELD_SIZE || middleName.length() >= NAME_FIELD_SIZE ||
      vehicle.length() >= VEHICLE_FIELD_SIZE ||
      birthDate.length() >= BIRTH_DATE_FIELD_SIZE) return false;

  CardRecord &record = cardRecords[cardCount++];
  memset(&record, 0, sizeof(record));
  copyCardField(record.uid, sizeof(record.uid), normalizedUID);
  copyCardField(record.lastName, sizeof(record.lastName), lastName);
  copyCardField(record.firstName, sizeof(record.firstName), firstName);
  copyCardField(record.middleName, sizeof(record.middleName), middleName);
  copyCardField(record.vehicle, sizeof(record.vehicle), vehicle);
  copyCardField(record.birthDate, sizeof(record.birthDate), birthDate);
  record.relayMask = relayMask;
  if (saveCardRecords()) return true;

  cardCount--;
  Serial.println("Не вдалося записати список карток у SPIFFS.");
  return false;
}

bool updateCardRecord(size_t index, uint8_t relayMask, const String &lastName,
                      const String &firstName, const String &middleName,
                      const String &vehicle, const String &birthDate) {
  if (!cardStorageReady || index >= cardCount || relayMask < 1 || relayMask > 3 ||
      lastName.length() >= NAME_FIELD_SIZE ||
      firstName.length() >= NAME_FIELD_SIZE || middleName.length() >= NAME_FIELD_SIZE ||
      vehicle.length() >= VEHICLE_FIELD_SIZE ||
      birthDate.length() >= BIRTH_DATE_FIELD_SIZE) return false;

  CardRecord previous = cardRecords[index];
  CardRecord &record = cardRecords[index];
  copyCardField(record.lastName, sizeof(record.lastName), lastName);
  copyCardField(record.firstName, sizeof(record.firstName), firstName);
  copyCardField(record.middleName, sizeof(record.middleName), middleName);
  copyCardField(record.vehicle, sizeof(record.vehicle), vehicle);
  copyCardField(record.birthDate, sizeof(record.birthDate), birthDate);
  record.relayMask = relayMask;
  if (saveCardRecords()) return true;

  cardRecords[index] = previous;
  Serial.println("Не вдалося зберегти зміни картки у SPIFFS.");
  return false;
}

bool removeCardRecord(size_t index) {
  if (!cardStorageReady || index >= cardCount) return false;
  for (size_t i = index; i + 1 < cardCount; i++) cardRecords[i] = cardRecords[i + 1];
  cardCount--;
  if (saveCardRecords()) return true;
  if (!loadCardFile(CARD_FILE)) {
    cardStorageReady = false;
    Serial.println("Помилка відновлення списку карток після невдалого видалення.");
  }
  return false;
}

bool removeUIDRecord(const String &uid) {
  if (!cardStorageReady) return false;
  String normalizedUID = normalizeUIDInput(uid);
  bool removed = false;
  for (size_t i = 0; i < cardCount;) {
    if (normalizedUID == cardRecords[i].uid) {
      if (!removeCardRecord(i)) return false;
      removed = true;
    } else {
      i++;
    }
  }
  return removed;
}

uint8_t findUIDRelayMask(const String &uid) {
  String normalizedUID = normalizeUIDInput(uid);
  for (size_t i = 0; i < cardCount; i++) {
    if (normalizedUID == cardRecords[i].uid) return cardRecords[i].relayMask;
  }
  return 0;
}

String getListJSON() {
  String json;
  json.reserve(cardCount * 220 + 2);
  json = "[";
  JsonDocument doc;
  for (size_t i = 0; i < cardCount; i++) {
    if (i > 0) json += ",";
    doc.clear();
    JsonObject item = doc.to<JsonObject>();
    item["index"] = i;
    item["uid"] = cardRecords[i].uid;
    item["relay"] = cardRecords[i].relayMask;
    item["lastName"] = cardRecords[i].lastName;
    item["firstName"] = cardRecords[i].firstName;
    item["middleName"] = cardRecords[i].middleName;
    item["vehicle"] = cardRecords[i].vehicle;
    item["birthDate"] = cardRecords[i].birthDate;
    item["readCount"] = cardRecords[i].readCount;
    item["lastReadEpoch"] = cardLastReadEpoch(cardRecords[i]);
    String recordJson;
    serializeJson(doc, recordJson);
    json += recordJson;
  }
  json += "]";
  return json;
}

bool saveAttendanceEvents() {
  cardFS.remove(ATTENDANCE_TEMP_FILE);
  File file = cardFS.open(ATTENDANCE_TEMP_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("SPIFFS: не вдалося відкрити файл журналу прибуття/вибуття.");
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < attendanceEventCount && ok; i++) {
    JsonDocument doc;
    doc["uid"] = attendanceEvents[i].uid;
    doc["epoch"] = attendanceEventEpoch(attendanceEvents[i]);
    doc["arrived"] = attendanceEvents[i].arrived;
    ok = serializeJson(doc, file) > 0 && file.write('\n') == 1;
  }
  file.close();
  if (!ok) {
    cardFS.remove(ATTENDANCE_TEMP_FILE);
    Serial.println("SPIFFS: помилка запису журналу прибуття/вибуття.");
    return false;
  }

  cardFS.remove(ATTENDANCE_BACKUP_FILE);
  bool hadCurrent = cardFS.exists(ATTENDANCE_FILE);
  if (hadCurrent && !cardFS.rename(ATTENDANCE_FILE, ATTENDANCE_BACKUP_FILE)) {
    cardFS.remove(ATTENDANCE_TEMP_FILE);
    Serial.println("SPIFFS: не вдалося створити резервну копію журналу.");
    return false;
  }
  if (!cardFS.rename(ATTENDANCE_TEMP_FILE, ATTENDANCE_FILE)) {
    if (hadCurrent) cardFS.rename(ATTENDANCE_BACKUP_FILE, ATTENDANCE_FILE);
    Serial.println("SPIFFS: не вдалося замінити журнал прибуття/вибуття.");
    return false;
  }
  if (hadCurrent) cardFS.remove(ATTENDANCE_BACKUP_FILE);
  return true;
}

bool initializeAttendanceStorage() {
  if (!cardFS.exists(ATTENDANCE_FILE)) {
    const char *recoveryFile = cardFS.exists(ATTENDANCE_BACKUP_FILE)
        ? ATTENDANCE_BACKUP_FILE : ATTENDANCE_TEMP_FILE;
    if ((cardFS.exists(ATTENDANCE_BACKUP_FILE) ||
         cardFS.exists(ATTENDANCE_TEMP_FILE)) &&
        !cardFS.rename(recoveryFile, ATTENDANCE_FILE)) {
      Serial.println("Не вдалося відновити журнал прибуття/вибуття.");
      return false;
    }
    if (!cardFS.exists(ATTENDANCE_FILE)) return true;
  }

  File file = cardFS.open(ATTENDANCE_FILE, FILE_READ);
  if (!file) {
    Serial.println("Не вдалося відкрити журнал прибуття/вибуття.");
    return false;
  }

  attendanceEventCount = 0;
  size_t invalidRows = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() == 0) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line)) {
      invalidRows++;
      continue;
    }
    String uid = normalizeUIDInput(String(doc["uid"] | ""));
    if (!isValidUIDFormat(uid) || !doc["arrived"].is<bool>()) {
      invalidRows++;
      continue;
    }

    AttendanceEvent event;
    memset(&event, 0, sizeof(event));
    copyCardField(event.uid, sizeof(event.uid), uid);
    setAttendanceEventEpoch(event, doc["epoch"] | 0UL);
    event.arrived = doc["arrived"].as<bool>();

    size_t matchingTypeCount = 0;
    for (size_t i = 0; i < attendanceEventCount; i++) {
      if (attendanceEvents[i].arrived == event.arrived) matchingTypeCount++;
    }
    if (matchingTypeCount == MAX_ATTENDANCE_EVENTS_PER_TYPE) {
      for (size_t i = 0; i < attendanceEventCount; i++) {
        if (attendanceEvents[i].arrived == event.arrived) {
          for (size_t j = i; j + 1 < attendanceEventCount; j++) {
            attendanceEvents[j] = attendanceEvents[j + 1];
          }
          attendanceEventCount--;
          break;
        }
      }
    }
    if (attendanceEventCount < MAX_ATTENDANCE_EVENTS_PER_TYPE * 2) {
      attendanceEvents[attendanceEventCount++] = event;
    }
  }
  file.close();
  if (invalidRows > 0) {
    Serial.print("Некоректних рядків у журналі прибуття/вибуття: ");
    Serial.println(invalidRows);
  }
  return true;
}

void trackCardRead(const String &uid) {
  time_t now;
  time(&now);
  uint32_t epoch = now >= 1704067200 ? (uint32_t)now : 0;

  for (size_t i = 0; i < cardCount; i++) {
    if (uid == cardRecords[i].uid) {
      if (cardRecords[i].readCount < UINT16_MAX) cardRecords[i].readCount++;
      if (epoch != 0) setCardLastReadEpoch(cardRecords[i], epoch);
      cardStatsDirty = true;
      break;
    }
  }

  size_t existingIndex = recentScanCount;
  for (size_t i = 0; i < recentScanCount; i++) {
    if (uid == recentScanUIDs[i]) {
      existingIndex = i;
      break;
    }
  }
  if (existingIndex == recentScanCount && recentScanCount < RECENT_SCAN_LIMIT) {
    recentScanCount++;
  }
  size_t moveCount = min(existingIndex, (size_t)(RECENT_SCAN_LIMIT - 1));
  while (moveCount > 0) {
    memcpy(recentScanUIDs[moveCount], recentScanUIDs[moveCount - 1], RECENT_UID_FIELD_SIZE);
    moveCount--;
  }
  memset(recentScanUIDs[0], 0, RECENT_UID_FIELD_SIZE);
  copyCardField(recentScanUIDs[0], RECENT_UID_FIELD_SIZE, uid);
}

void seedRecentScans() {
  bool selected[MAX_CARDS] = {};
  for (size_t shown = 0; shown < RECENT_SCAN_LIMIT; shown++) {
    size_t selectedIndex = cardCount;
    uint32_t latestEpoch = 0;
    for (size_t i = 0; i < cardCount; i++) {
      uint32_t readEpoch = cardLastReadEpoch(cardRecords[i]);
      if (!selected[i] && cardRecords[i].readCount > 0 &&
          selectedIndex == cardCount) {
        selectedIndex = i;
        latestEpoch = readEpoch;
      } else if (!selected[i] && cardRecords[i].readCount > 0 &&
                 readEpoch > latestEpoch) {
        selectedIndex = i;
        latestEpoch = readEpoch;
      }
    }
    if (selectedIndex == cardCount) break;
    selected[selectedIndex] = true;
    copyCardField(recentScanUIDs[recentScanCount], RECENT_UID_FIELD_SIZE,
                  String(cardRecords[selectedIndex].uid));
    recentScanCount++;
  }
}

String getActivityJSON() {
  JsonDocument doc;
  time_t now;
  time(&now);
  bool timeSynced = now >= 1704067200;
  doc["timeSynced"] = timeSynced;
  doc["epoch"] = timeSynced ? (uint32_t)now : 0;

  JsonArray recent = doc["recent"].to<JsonArray>();
  for (size_t i = 0; i < recentScanCount; i++) {
    JsonObject item = recent.add<JsonObject>();
    item["uid"] = recentScanUIDs[i];
    for (size_t j = 0; j < cardCount; j++) {
      if (strcmp(recentScanUIDs[i], cardRecords[j].uid) == 0) {
        item["epoch"] = cardLastReadEpoch(cardRecords[j]);
        item["vehicle"] = cardRecords[j].vehicle;
        item["lastName"] = cardRecords[j].lastName;
        break;
      }
    }
  }

  JsonArray arrivals = doc["arrivals"].to<JsonArray>();
  JsonArray departures = doc["departures"].to<JsonArray>();
  size_t arrivalCount = 0;
  size_t departureCount = 0;
  for (size_t i = attendanceEventCount; i > 0; i--) {
    const AttendanceEvent &event = attendanceEvents[i - 1];
    JsonArray target = event.arrived ? arrivals : departures;
    size_t &count = event.arrived ? arrivalCount : departureCount;
    if (count >= RECENT_SCAN_LIMIT) continue;
    JsonObject item = target.add<JsonObject>();
    item["uid"] = event.uid;
    item["epoch"] = attendanceEventEpoch(event);
    for (size_t j = 0; j < cardCount; j++) {
      if (strcmp(event.uid, cardRecords[j].uid) == 0) {
        item["vehicle"] = cardRecords[j].vehicle;
        item["lastName"] = cardRecords[j].lastName;
        break;
      }
    }
    count++;
  }

  String json;
  serializeJson(doc, json);
  return json;
}

// -------------------- WiFi / ETH фолбек --------------------
const unsigned long ETH_WAIT_MS = 8000;
const unsigned long WIFI_STA_TIMEOUT = 5000;
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

String readWiFiSSID() { return eepromReadString(ADDR_WIFI_SSID, WIFI_MAX_SSID); }
String readWiFiPass() { return eepromReadString(ADDR_WIFI_PASS, WIFI_MAX_PASS); }
void writeWiFiCreds(const String &ssid, const String &pass) {
  eepromWriteString(ADDR_WIFI_SSID, ssid, WIFI_MAX_SSID);
  eepromWriteString(ADDR_WIFI_PASS, pass, WIFI_MAX_PASS);
  EEPROM.commit();
}

String readMqttHost() { return eepromReadString(ADDR_MQTT_HOST, MQTT_MAX_HOST); }
String readMqttUser() { return eepromReadString(ADDR_MQTT_USER, MQTT_MAX_USER); }
String readMqttPass() { return eepromReadString(ADDR_MQTT_PASS, MQTT_MAX_PASS); }
uint16_t readMqttPort() {
  uint16_t port = 0;
  EEPROM.get(ADDR_MQTT_PORT, port);
  return (port == 0 || port == 0xFFFF) ? 1883 : port;
}
String readMqttTopic() {
  String topic = eepromReadString(ADDR_MQTT_TOPIC, MQTT_MAX_TOPIC);
  return topic.length() ? topic : String("wt32/controller");
}

void pulseRelays(uint8_t mask, uint32_t durationMs) {
  durationMs = constrain(durationMs, 50UL, 60000UL);
  unsigned long offAt = millis() + durationMs;
  if (mask & 0x01) {
    digitalWrite(RELAY_1, HIGH);
    relay1_active = true;
    relay1_turnOffTime = offAt;
  }
  if (mask & 0x02) {
    digitalWrite(RELAY_2, HIGH);
    relay2_active = true;
    relay2_turnOffTime = offAt;
  }
}

void stopRelays(uint8_t mask) {
  if (mask & 0x01) {
    digitalWrite(RELAY_1, LOW);
    relay1_active = false;
  }
  if (mask & 0x02) {
    digitalWrite(RELAY_2, LOW);
    relay2_active = false;
  }
}

String mqttTopic(const char *suffix) {
  return readMqttTopic() + "/" + suffix;
}

void publishMqttStatus() {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["online"] = true;
  doc["network"] = (currentNet == NET_ETH) ? "Ethernet" :
                   (currentNet == NET_WIFI_STA) ? "WiFi" :
                   (currentNet == NET_WIFI_AP) ? "AP" : "offline";
  doc["ip"] = (currentNet == NET_ETH) ? ETH.localIP().toString() :
              (currentNet == NET_WIFI_STA) ? WiFi.localIP().toString() :
              (currentNet == NET_WIFI_AP) ? WiFi.softAPIP().toString() : "";
  doc["pn532"] = nfcPresent;
  doc["relay1"] = relay1_active;
  doc["relay2"] = relay2_active;
  char payload[256];
  size_t length = serializeJson(doc, payload, sizeof(payload));
  if (length == 0 || !mqttClient.publish(mqttTopic("status").c_str(), payload, true)) {
    Serial.println("Не вдалося опублікувати MQTT status.");
  }
  lastPublishedNfcPresent = nfcPresent;
  lastPublishedNet = currentNet;
  lastMqttStatusPublish = millis();
}

void publishMqttCard(const String &uid, uint8_t relayMask) {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["uid"] = uid;
  doc["authorized"] = relayMask != 0;
  doc["relay"] = relayMask;
  char payload[128];
  size_t length = serializeJson(doc, payload, sizeof(payload));
  if (length == 0 || !mqttClient.publish(mqttTopic("event/card").c_str(), payload)) {
    Serial.println("Не вдалося опублікувати MQTT card event.");
  }
}

void publishMqttCommandResult(const char *command, bool ok) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["command"] = command;
  doc["ok"] = ok;
  char payload[96];
  size_t length = serializeJson(doc, payload, sizeof(payload));
  if (length == 0 || !mqttClient.publish(mqttTopic("result").c_str(), payload)) {
    Serial.println("Не вдалося опублікувати MQTT command result.");
  }
}

void handleMqttMessage(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("Некоректний MQTT JSON: ");
    Serial.println(error.c_str());
    return;
  }

  String receivedTopic(topic);
  if (receivedTopic == mqttTopic("cmd/relay")) {
    const char *action = doc["action"] | "";
    int mask = doc["mask"] | 0;
    if (mask < 1 || mask > 3) {
      publishMqttCommandResult("relay", false);
      return;
    }
    if (strcmp(action, "stop") == 0) {
      stopRelays((uint8_t)mask);
    } else if (strcmp(action, "pulse") == 0) {
      int duration = doc["duration"] | clickDuration;
      if (duration < 50 || duration > 60000) {
        publishMqttCommandResult("relay", false);
        return;
      }
      pulseRelays((uint8_t)mask, (uint32_t)duration);
    } else {
      publishMqttCommandResult("relay", false);
      return;
    }
    publishMqttCommandResult("relay", true);
    publishMqttStatus();
    return;
  }

  if (receivedTopic == mqttTopic("cmd/card/add") ||
      receivedTopic == mqttTopic("cmd/card/remove")) {
    const char *uidValue = doc["uid"] | "";
    String uid = normalizeUIDInput(String(uidValue));
    if (!isValidUIDFormat(uid)) {
      publishMqttCommandResult("card", false);
      return;
    }

    bool added = false;
    if (receivedTopic == mqttTopic("cmd/card/add")) {
      int relay = doc["relay"] | 1;
      if (relay < 1 || relay > 3) {
        publishMqttCommandResult("card", false);
        return;
      }
      added = addRecord(uid, (uint8_t)relay);
    } else {
      added = removeUIDRecord(uid);
    }
    publishMqttCommandResult("card", added);
  }
}

void serviceMqtt(unsigned long now) {
  String host = readMqttHost();
  if (host.length() == 0 ||
      (currentNet != NET_ETH && currentNet != NET_WIFI_STA)) {
    if (mqttClient.connected()) mqttClient.disconnect();
    return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    if (now - lastMqttStatusPublish >= MQTT_STATUS_INTERVAL ||
        lastPublishedNfcPresent != nfcPresent ||
        lastPublishedNet != currentNet) {
      publishMqttStatus();
    }
    return;
  }

  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL) return;
  lastMqttAttempt = now;

  String baseTopic = readMqttTopic();
  String statusTopic = baseTopic + "/status";
  String clientId = "WT32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String user = readMqttUser();
  String pass = readMqttPass();
  mqttServerHost = host;
  mqttClient.setServer(mqttServerHost.c_str(), readMqttPort());
  mqttClient.setSocketTimeout(2);

  bool connected = user.length() > 0
      ? mqttClient.connect(clientId.c_str(), user.c_str(), pass.c_str(),
                           statusTopic.c_str(), 0, true, "{\"online\":false}")
      : mqttClient.connect(clientId.c_str(), statusTopic.c_str(), 0, true,
                           "{\"online\":false}");
  if (!connected) {
    Serial.print("MQTT підключення не вдалося, код: ");
    Serial.println(mqttClient.state());
    return;
  }

  Serial.print("MQTT підключено до ");
  Serial.println(host);
  mqttClient.setCallback(handleMqttMessage);
  bool subscriptionsOk =
      mqttClient.subscribe(mqttTopic("cmd/relay").c_str()) &&
      mqttClient.subscribe(mqttTopic("cmd/card/add").c_str()) &&
      mqttClient.subscribe(mqttTopic("cmd/card/remove").c_str());
  if (!subscriptionsOk) Serial.println("Не вдалося оформити всі MQTT-підписки.");
  publishMqttStatus();
}

void serviceRelays(unsigned long now) {
  bool changed = false;
  if (relay1_active && (int32_t)(now - relay1_turnOffTime) >= 0) {
    stopRelays(0x01);
    Serial.println("Relay 1 OFF");
    changed = true;
  }
  if (relay2_active && (int32_t)(now - relay2_turnOffTime) >= 0) {
    stopRelays(0x02);
    Serial.println("Relay 2 OFF");
    changed = true;
  }
  if (changed) publishMqttStatus();
}

void startWiFiAP(const char* apSSID, const char* apPass);
void stopWiFiAP(bool keepStation = false);

bool hasIP(const IPAddress &ip) {
  return ip != INADDR_NONE && ip != IPAddress(0, 0, 0, 0);
}

bool initializePN532() {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    nfcPresent = false;
    return false;
  }
  nfc.SAMConfig();
  nfcPresent = true;
  Serial.println("PN532 знайдено");
  return true;
}

void serviceNetwork(unsigned long now) {
  if (now - lastNetworkCheck < 500) return;
  lastNetworkCheck = now;

  IPAddress ethIP = ETH.localIP();
  if (ETH.linkUp() && hasIP(ethIP)) {
    wifiConnectPending = false;
    if (apActive) stopWiFiAP();
    else if (currentNet != NET_ETH && WiFi.getMode() != WIFI_OFF) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("Ethernet активний; WiFi вимкнено до втрати Ethernet-з’єднання.");
    }
    if (currentNet != NET_ETH) {
      Serial.print("Ethernet підключено, IP: "); Serial.println(ethIP);
      Serial.print("Вебсторінка: http://"); Serial.println(ethIP);
    }
    currentNet = NET_ETH;
    networkStartup = NET_START_DONE;
    return;
  }

  if (wifiConnectPending && (int32_t)(now - wifiConnectPendingAt) >= 0) {
    wifiConnectPending = false;
    WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);
    String ssid = readWiFiSSID();
    String pass = readWiFiPass();
    currentNet = apActive ? NET_WIFI_AP : NET_NONE;
    Serial.print("Підключаюся до WiFi SSID: ");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
    networkPhaseStarted = now;
    networkStartup = NET_START_WIFI;
    return;
  }

  if (currentNet == NET_ETH) {
    if (WiFi.status() == WL_CONNECTED) {
      currentNet = NET_WIFI_STA;
      Serial.print("Перемикаюсь на WiFi, IP: "); Serial.println(WiFi.localIP());
      return;
    }
    String ssid = readWiFiSSID();
    if (ssid.length() > 0) {
      Serial.println("Ethernet втрачено; пробую збережену WiFi-мережу.");
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      String pass = readWiFiPass();
      WiFi.begin(ssid.c_str(), pass.c_str());
      networkPhaseStarted = now;
      networkStartup = NET_START_WIFI;
      currentNet = NET_NONE;
      return;
    }
    Serial.println("Ethernet втрачено, WiFi не налаштований; запускаю AP.");
    startWiFiAP("WT32-Config", "config1234");
    networkStartup = NET_START_DONE;
    return;
  }
  if (currentNet == NET_WIFI_STA && WiFi.status() != WL_CONNECTED) {
    Serial.println("Мережа втрачена; запускаю локальну точку доступу.");
    startWiFiAP("WT32-Config", "config1234");
    networkStartup = NET_START_DONE;
    return;
  }

  if (networkStartup == NET_START_ETH) {
    if (networkPhaseStarted == 0) networkPhaseStarted = now;
    if (now - networkPhaseStarted < ETH_WAIT_MS) return;

    String ssid = readWiFiSSID();
    if (ssid.length() == 0) {
      Serial.println("SSID не знайдено в EEPROM; вмикаю AP.");
      startWiFiAP("WT32-Config", "config1234");
      networkStartup = NET_START_DONE;
      return;
    }

    Serial.print("Пробую WiFi SSID: "); Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    String pass = readWiFiPass();
    WiFi.begin(ssid.c_str(), pass.c_str());
    networkPhaseStarted = now;
    networkStartup = NET_START_WIFI;
    return;
  }

  if (networkStartup == NET_START_WIFI) {
    if (WiFi.status() == WL_CONNECTED) {
      if (apActive) stopWiFiAP(true);
      currentNet = NET_WIFI_STA;
      networkStartup = NET_START_DONE;
      Serial.print("WiFi підключено, IP: "); Serial.println(WiFi.localIP());
      Serial.print("Вебсторінка: http://"); Serial.println(WiFi.localIP());
      return;
    }
    if (now - networkPhaseStarted >= WIFI_STA_TIMEOUT) {
      Serial.println("WiFi не підключився; запускаю AP.");
      if (apActive) {
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
        currentNet = NET_WIFI_AP;
      } else {
        WiFi.disconnect(true);
        startWiFiAP("WT32-Config", "config1234");
      }
      networkStartup = NET_START_DONE;
    }
  }
}

// Запуск AP для конфігурації
void startWiFiAP(const char* apSSID = "WT32-Config", const char* apPass = "config1234") {
  if (apActive) {
    if (!(ETH.linkUp() && hasIP(ETH.localIP()))) currentNet = NET_WIFI_AP;
    return;
  }

  Serial.println("Запуск WiFi AP для конфігурації...");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
    Serial.println("Не вдалося налаштувати IP для AP.");
    return;
  }
  bool ok = WiFi.softAP(apSSID, apPass);
  if (ok) {
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP запущено: ");
    Serial.print(apSSID);
    Serial.print(" IP: ");
    Serial.println(apIP);
    Serial.print("Вебсторінка: http://"); Serial.println(apIP);
    captiveDnsActive = dnsServer.start(53, "*", AP_IP);
    apActive = true;
    if (!(ETH.linkUp() && hasIP(ETH.localIP()))) currentNet = NET_WIFI_AP;
    if (captiveDnsActive) {
      Serial.println("Captive portal DNS активний; HTTP-адреси перенаправляються на сторінку входу.");
    } else {
      Serial.println("DNS captive portal не запустився; відкрийте http://192.168.4.1 вручну.");
    }
  } else {
    Serial.println("Не вдалося запустити AP.");
  }
}

void stopWiFiAP(bool keepStation) {
  if (captiveDnsActive) dnsServer.stop();
  captiveDnsActive = false;
  if (apActive) WiFi.softAPdisconnect(false);
  apActive = false;
  WiFi.mode(keepStation ? WIFI_STA : WIFI_OFF);
}

void handleNotFound() {
  if (apActive) {
    AsyncWebServerResponse *response =
        server.request->beginResponse(302, "text/plain", "Перехід на сторінку пристрою");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Location", "/");
    server.request->send(response);
    return;
  }
  server.send(404, "text/plain", "Сторінку не знайдено");
}

// -------------------- Веб-обробники --------------------
void handleRootLegacy() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  String html = "<!doctype html><html lang='uk'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'><title>RFID і реле</title>";
  html += "<style>body{font:16px Arial,sans-serif;margin:16px;background:#f3f5f7;color:#202124}.card{background:white;padding:16px;border-radius:10px;margin:0 auto 14px;max-width:1100px;box-shadow:0 1px 5px #0002}button,input,select{font:inherit;padding:9px;margin:4px 2px}button{cursor:pointer}input,select{box-sizing:border-box;max-width:100%}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:8px;border-bottom:1px solid #ddd}th{position:sticky;top:0;background:#f3f5f7;z-index:1}.tableWrap{max-height:55vh;overflow:auto;border:1px solid #ddd}.row{display:flex;gap:8px;flex-wrap:wrap}.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:4px}.fields label{display:flex;flex-direction:column}#wifiLoading{position:fixed;inset:0;z-index:10;background:#f3f5f7;display:none;align-items:center;justify-content:center;text-align:center}#wifiLoading.visible{display:flex}.loadingCard{background:#fff;padding:20px;border-radius:10px;box-shadow:0 1px 5px #0002}.spinner{width:22px;height:22px;margin:0 auto 12px;border:3px solid #ddd;border-top-color:#2463eb;border-radius:50%;animation:spin .8s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}</style>";
  html += "<script>let cancelWifiWait=false;async function openWifi(event){event.preventDefault();cancelWifiWait=false;let overlay=document.getElementById('wifiLoading'),status=document.getElementById('wifiLoadingStatus');overlay.classList.add('visible');while(!cancelWifiWait){let controller=new AbortController(),timer=setTimeout(()=>controller.abort(),1800);try{let response=await fetch('/wifi',{cache:'no-store',signal:controller.signal});clearTimeout(timer);if(response.status===401){location.href='/wifi';return}if(response.ok){location.href='/wifi';return}status.textContent='Сторінка ще готується. Повторна спроба...'}catch(e){clearTimeout(timer);status.textContent='Очікую відповіді пристрою...'}await new Promise(resolve=>setTimeout(resolve,400))}overlay.classList.remove('visible')}function cancelWifiWaitNow(){cancelWifiWait=true}async function post(url,data){let r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});return r.json()}async function refreshStatus(){let r=await fetch('/api/status');let s=await r.json();document.getElementById('lastUid').textContent=s.uid||'ще не зчитувалась';document.getElementById('nfcStatus').textContent=s.pn532?'підключено':'не знайдено';document.getElementById('netStatus').textContent=s.network+' '+s.ip}async function refresh(){let r=await fetch('/api/list');let a=await r.json();let t=document.getElementById('list');t.replaceChildren();for(let x of a){let tr=document.createElement('tr');for(let v of [x.index,x.uid,x.relay]){let td=document.createElement('td');td.textContent=v;tr.appendChild(td)}let td=document.createElement('td'),b=document.createElement('button');b.textContent='Видалити';b.onclick=async()=>{let q=await post('/api/remove',{index:x.index});alert(q.msg);refresh()};td.appendChild(b);tr.appendChild(td);t.appendChild(tr)}if(!a.length)t.innerHTML='<tr><td colspan=4>Список порожній</td></tr>';refreshStatus()}async function add(){let q=await post('/api/add',{uid:document.getElementById('uid').value,relay:document.getElementById('relay').value});alert(q.msg);refresh()}async function addLast(){let q=await post('/api/add-last',{relay:document.getElementById('relay').value});alert(q.msg);refresh()}async function pulse(mask){let q=await post('/api/relay',{action:'pulse',mask:mask,duration:document.getElementById('duration').value});alert(q.msg)}async function stop(mask){let q=await post('/api/relay',{action:'stop',mask:mask});alert(q.msg)}async function saveDuration(){let q=await post('/api/settings',{duration:document.getElementById('duration').value});alert(q.msg)}window.onload=()=>{refresh();setInterval(refreshStatus,1500);setInterval(refresh,5000)};</script></head><body>";
  html += "<div id='wifiLoading' aria-live='polite'><div class='loadingCard'><div class='spinner'></div><strong>Відкриваю налаштування WiFi</strong><p id='wifiLoadingStatus'>Очікую відповіді пристрою...</p><button type='button' onclick='cancelWifiWaitNow()'>Скасувати</button></div></div>";
  html += "<h2 style='max-width:850px;margin:16px auto'>RFID і керування реле</h2>";
  html += "<section class='card'><h3>Стан пристрою</h3><p>Остання картка: <strong>";
  html += "<span id='lastUid'>";
  html += (lastScannedUID.length() ? lastScannedUID : String("ще не зчитувалась"));
  html += "</span></strong></p><p>PN532: <span id='nfcStatus'>";
  html += (nfcPresent ? "підключено" : "не знайдено");
  String netStatus = (currentNet == NET_ETH) ? "Ethernet" : (currentNet == NET_WIFI_STA) ? "WiFi роутер" : (currentNet == NET_WIFI_AP) ? "точка доступу WT32-Config" : "автономно, без мережі";
  html += "</span></p><p>Мережа: <strong id='netStatus'>" + netStatus + "</strong></p></section>";
  html += "<section class='card'><h3>Керування реле</h3><label>Імпульс, мс: <input id='duration' type='number' min='50' max='60000' value='" + String(clickDuration) + "'></label><button onclick='saveDuration()'>Зберегти</button><div class='row'><button onclick='pulse(1)'>Імпульс реле 1</button><button onclick='pulse(2)'>Імпульс реле 2</button><button onclick='pulse(3)'>Обидва реле</button></div><div class='row'><button onclick='stop(1)'>Вимкнути 1</button><button onclick='stop(2)'>Вимкнути 2</button><button onclick='stop(3)'>Вимкнути обидва</button></div></section>";
  html += "<section class='card'><h3>Картки та власники</h3><label for='cardSearch'>Пошук за прізвищем, ім’ям/ініціалами, номером авто або датою народження:</label><input id='cardSearch' type='search' style='width:100%' placeholder='Наприклад: Петренко ІП, АА1234ВК або 1990-05-20'><p id='cardListStatus'>Завантаження списку...</p><div class='tableWrap'><table><thead><tr><th>#</th><th>UID</th><th>Прізвище</th><th>Ім’я</th><th>По батькові</th><th>Номер авто</th><th>Дата народження</th><th>Реле</th><th></th></tr></thead><tbody id='list'></tbody></table></div></section>";
  html += "<section class='card'><h3>Додати картку</h3><div class='fields'><label>UID картки<input id='uid' type='text' maxlength='20' placeholder='AA BB CC DD' required></label><label>Прізвище<input id='lastName' maxlength='63'></label><label>Ім’я<input id='firstName' maxlength='63'></label><label>По батькові<input id='middleName' maxlength='63'></label><label>Номер автомобіля<input id='vehicle' maxlength='23' placeholder='AA1234BB'></label><label>Дата народження<input id='birthDate' type='date'></label><label>Реле<select id='relay'><option value='1'>1</option><option value='2'>2</option><option value='3'>1 і 2</option></select></label></div><div class='row'><button id='addCardButton' onclick='add()'>Додати картку</button><button type='button' onclick='fillLastUID()'>Підставити останній зчитаний UID</button></div><p id='uidAddStatus' role='status'>Дозволено до 300 карток. Особисті дані можна залишити порожніми.</p><p>Апаратна кнопка додає останню картку без персональних даних для реле 1.</p></section>";
  html += "<script>let cardData=[];function renderCards(){let wrap=document.querySelector('.tableWrap'),oldTop=wrap.scrollTop,oldLeft=wrap.scrollLeft;let normalize=value=>String(value||'').toLocaleLowerCase().replace(/[.\\s-]+/g,'');let query=normalize(document.getElementById('cardSearch').value);let visible=cardData.filter(x=>{let initials=[x.firstName,x.middleName].map(value=>Array.from(value||'')[0]||'').join('');let dateParts=(x.birthDate||'').split('-');let localDate=dateParts.length===3?dateParts[2]+'.'+dateParts[1]+'.'+dateParts[0]:'';let searchable=[x.lastName,x.firstName,x.middleName,x.vehicle,x.birthDate,localDate,x.uid,initials,(x.lastName||'')+initials].map(normalize).join(' ');return searchable.includes(query)});let body=document.getElementById('list');body.replaceChildren();for(let x of visible){let row=document.createElement('tr');for(let value of [x.index,x.uid,x.lastName,x.firstName,x.middleName,x.vehicle,x.birthDate,x.relay]){let cell=document.createElement('td');cell.textContent=value||'';row.appendChild(cell)}let cell=document.createElement('td'),button=document.createElement('button');button.textContent='Видалити';button.onclick=async()=>{if(!confirm('Видалити картку '+x.uid+'?'))return;let result=await post('/api/remove',{index:x.index});alert(result.msg);await refresh()};cell.appendChild(button);row.appendChild(cell);body.appendChild(row)}wrap.scrollTop=oldTop;wrap.scrollLeft=oldLeft;document.getElementById('cardListStatus').textContent=visible.length?'Показано '+visible.length+' із '+cardData.length+' карток':'Карток за цим пошуком не знайдено'}window.refresh=async function(){try{let response=await fetch('/api/list',{cache:'no-store'});if(!response.ok)throw new Error('HTTP '+response.status);cardData=await response.json();renderCards();await refreshStatus()}catch(error){document.getElementById('cardListStatus').textContent='Не вдалося завантажити список. Перевірте з’єднання.'}};document.getElementById('cardSearch').addEventListener('input',()=>{document.querySelector('.tableWrap').scrollTop=0;renderCards()});window.add=async function(){let button=document.getElementById('addCardButton'),status=document.getElementById('uidAddStatus');let fields=['uid','lastName','firstName','middleName','vehicle','birthDate','relay'];let values={};for(let name of fields)values[name]=document.getElementById(name).value;button.disabled=true;status.textContent='Зберігаю картку...';try{let response=await fetch('/api/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(values)});let result=await response.json();status.textContent=result.msg||(response.ok?'Картку збережено':'Помилка додавання');if(response.ok&&result.ok){for(let name of fields.slice(0,6))document.getElementById(name).value='';await refresh()}}catch(error){status.textContent='Не вдалося виконати запит. Перевірте з’єднання та повторіть спробу.'}finally{button.disabled=false}};window.fillLastUID=async function(){try{let response=await fetch('/api/status',{cache:'no-store'});let result=await response.json();if(!result.uid){document.getElementById('uidAddStatus').textContent='Спочатку піднесіть картку до зчитувача.';return}document.getElementById('uid').value=result.uid;document.getElementById('lastName').focus()}catch(error){document.getElementById('uidAddStatus').textContent='Не вдалося отримати останній UID.'}};</script>";
  html += "<script>let editingCardIndex=null;let renderCardsOriginal=renderCards;renderCards=function(){renderCardsOriginal();for(const row of document.querySelectorAll('#list tr')){let index=Number(row.cells[0].textContent),record=cardData.find(item=>item.index===index);if(!record)continue;let edit=document.createElement('button');edit.textContent='Редагувати';edit.onclick=()=>{editingCardIndex=record.index;document.getElementById('uid').value=record.uid;document.getElementById('uid').readOnly=true;for(let field of ['lastName','firstName','middleName','vehicle','birthDate','relay'])document.getElementById(field).value=record[field]||'';let save=document.getElementById('addCardButton');save.textContent='Зберегти зміни';let cancel=document.getElementById('cancelEditButton');if(!cancel){cancel=document.createElement('button');cancel.id='cancelEditButton';cancel.type='button';cancel.textContent='Скасувати редагування';save.after(cancel)}cancel.onclick=()=>{editingCardIndex=null;document.getElementById('uid').readOnly=false;save.textContent='Додати картку';cancel.remove()};document.getElementById('uidAddStatus').textContent='Редагування картки '+record.uid;save.closest('.card').scrollIntoView({behavior:'smooth'})};row.lastElementChild.prepend(edit)}};window.add=async function(){let button=document.getElementById('addCardButton'),status=document.getElementById('uidAddStatus'),editing=editingCardIndex!==null;let fields=['uid','lastName','firstName','middleName','vehicle','birthDate','relay'];let values={};for(let name of fields)values[name]=document.getElementById(name).value;if(editing)values.index=editingCardIndex;button.disabled=true;status.textContent=editing?'Зберігаю зміни картки...':'Зберігаю картку...';try{let response=await fetch(editing?'/api/update':'/api/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(values)});let result=await response.json();status.textContent=result.msg||(response.ok?'Картку збережено':'Помилка збереження');if(response.ok&&result.ok){for(let name of fields.slice(0,6))document.getElementById(name).value='';if(editing){editingCardIndex=null;document.getElementById('uid').readOnly=false;button.textContent='Додати картку';document.getElementById('cancelEditButton').remove()}await refresh()}}catch(error){status.textContent='Не вдалося виконати запит. Перевірте з’єднання та повторіть спробу.'}finally{button.disabled=false}};</script>";
  html += "<script>window.refresh=async function(){let status=document.getElementById('cardListStatus');try{let response=await fetch('/api/list',{cache:'no-store',credentials:'include'}),body=await response.text();if(!response.ok){let message='HTTP '+response.status;try{let result=JSON.parse(body);if(result.msg)message+=': '+result.msg}catch(e){}throw new Error(message)}let records=JSON.parse(body);if(!Array.isArray(records))throw new Error('сервер повернув не список карток');cardData=records;renderCards()}catch(error){status.textContent='Не вдалося завантажити список: '+error.message;console.error('Помилка завантаження списку карток:',error);return}try{await refreshStatus()}catch(error){console.error('Не вдалося оновити стан пристрою:',error)}};</script>";
  html += "<script>let listRequestInProgress=false;async function apiRequest(url,options={}){let controller=new AbortController(),timer=setTimeout(()=>controller.abort(),10000);try{return await fetch(url,{...options,cache:'no-store',credentials:'same-origin',signal:controller.signal})}finally{clearTimeout(timer)}}async function responseMessage(response){let text=await response.text();try{return JSON.parse(text)}catch(error){throw new Error('сервер повернув некоректну відповідь (HTTP '+response.status+')')}}window.refresh=async function(){if(listRequestInProgress)return;listRequestInProgress=true;let status=document.getElementById('cardListStatus');try{let response=await apiRequest('/api/list');if(!response.ok){let result=await responseMessage(response);throw new Error(result.msg||'HTTP '+response.status)}let records=await responseMessage(response);if(!Array.isArray(records))throw new Error('сервер повернув не список карток');cardData=records;renderCards()}catch(error){let message=error.name==='AbortError'?'час очікування відповіді пристрою вичерпано':error.message;status.textContent='Не вдалося завантажити список: '+message;console.error('Помилка завантаження списку карток:',error)}finally{listRequestInProgress=false}};window.add=async function(){let button=document.getElementById('addCardButton'),status=document.getElementById('uidAddStatus'),editing=editingCardIndex!==null;let fields=['uid','lastName','firstName','middleName','vehicle','birthDate','relay'],values={};for(let name of fields)values[name]=document.getElementById(name).value;if(editing)values.index=editingCardIndex;button.disabled=true;status.textContent=editing?'Зберігаю зміни картки...':'Зберігаю картку...';try{let response=await apiRequest(editing?'/api/update':'/api/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(values)}),result=await responseMessage(response);status.textContent=result.msg||(response.ok?'Картку збережено':'Помилка збереження');if(!response.ok||!result.ok)return;for(let name of fields.slice(0,6))document.getElementById(name).value='';if(editing){editingCardIndex=null;document.getElementById('uid').readOnly=false;button.textContent='Додати картку';let cancel=document.getElementById('cancelEditButton');if(cancel)cancel.remove()}await refresh()}catch(error){status.textContent=error.name==='AbortError'?'Пристрій не відповів за 10 секунд. Перевірте список перед повторним збереженням.':'Помилка запиту: '+error.message;console.error('Помилка збереження картки:',error)}finally{button.disabled=false}};window.onload=()=>{refresh();refreshStatus().catch(error=>console.error('Не вдалося отримати стан пристрою:',error));setInterval(()=>refreshStatus().catch(error=>console.error('Не вдалося оновити стан пристрою:',error)),5000)};</script>";
  html += "<script>const cardSearchInput=document.getElementById('cardSearch'),cardSearchForm=document.createElement('form');cardSearchForm.style.display='flex';cardSearchForm.style.gap='6px';cardSearchForm.style.flexWrap='wrap';cardSearchForm.style.margin='4px 0 12px';cardSearchInput.parentNode.insertBefore(cardSearchForm,cardSearchInput);cardSearchForm.appendChild(cardSearchInput);const cardSearchButton=document.createElement('button');cardSearchButton.type='submit';cardSearchButton.textContent='Знайти';cardSearchForm.appendChild(cardSearchButton);const cardSearchClear=document.createElement('button');cardSearchClear.type='button';cardSearchClear.textContent='Очистити';cardSearchClear.onclick=()=>{cardSearchInput.value='';renderCards();cardSearchInput.focus()};cardSearchForm.appendChild(cardSearchClear);cardSearchForm.onsubmit=event=>{event.preventDefault();renderCards()};cardSearchInput.addEventListener('input',()=>renderCards());renderCards=function(){const query=cardSearchInput.value.toLocaleLowerCase().replace(/[.\\s-]+/g,'');const body=document.getElementById('list');body.replaceChildren();const visible=cardData.filter(record=>{const initials=[record.firstName,record.middleName].map(value=>Array.from(value||'')[0]||'').join('');const parts=(record.birthDate||'').split('-'),localDate=parts.length===3?parts[2]+'.'+parts[1]+'.'+parts[0]:'';return [record.uid,record.lastName,record.firstName,record.middleName,record.vehicle,record.birthDate,localDate,initials,(record.lastName||'')+initials].some(value=>String(value||'').toLocaleLowerCase().replace(/[.\\s-]+/g,'').includes(query))});for(const record of visible){const row=document.createElement('tr');for(const value of [record.index,record.uid,record.lastName,record.firstName,record.middleName,record.vehicle,record.birthDate,record.relay]){const cell=document.createElement('td');cell.textContent=value||'';row.appendChild(cell)}const actions=document.createElement('td'),editButton=document.createElement('button'),deleteButton=document.createElement('button');editButton.type='button';editButton.textContent='Змінити';editButton.onclick=()=>{editingCardIndex=record.index;document.getElementById('uid').value=record.uid;document.getElementById('uid').readOnly=true;for(const field of ['lastName','firstName','middleName','vehicle','birthDate','relay'])document.getElementById(field).value=record[field]||'';const saveButton=document.getElementById('addCardButton');saveButton.textContent='Зберегти зміни';let cancelButton=document.getElementById('cancelEditButton');if(!cancelButton){cancelButton=document.createElement('button');cancelButton.id='cancelEditButton';cancelButton.type='button';cancelButton.textContent='Скасувати';saveButton.after(cancelButton)}cancelButton.onclick=()=>{editingCardIndex=null;document.getElementById('uid').readOnly=false;saveButton.textContent='Додати картку';cancelButton.remove()};document.getElementById('uidAddStatus').textContent='Редагування картки '+record.uid;saveButton.closest('.card').scrollIntoView({behavior:'smooth'})};deleteButton.type='button';deleteButton.textContent='Видалити';deleteButton.onclick=async()=>{if(!confirm('Видалити картку '+record.uid+'?'))return;deleteButton.disabled=true;try{const response=await apiRequest('/api/remove',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({index:record.index})}),result=await responseMessage(response);document.getElementById('cardListStatus').textContent=result.msg||'Картку видалено';if(response.ok&&result.ok)await refresh()}catch(error){document.getElementById('cardListStatus').textContent='Не вдалося видалити картку: '+error.message;deleteButton.disabled=false}};actions.append(editButton,deleteButton);row.appendChild(actions);body.appendChild(row)}document.getElementById('cardListStatus').textContent=visible.length?'Знайдено '+visible.length+' із '+cardData.length+' карток':'Карток за цим пошуком не знайдено'};window.refresh=async function(){if(listRequestInProgress)return;listRequestInProgress=true;const status=document.getElementById('cardListStatus');try{const response=await apiRequest('/api/list');if(!response.ok){const error=await responseMessage(response);throw new Error(error.msg||'HTTP '+response.status)}const records=await responseMessage(response);if(!Array.isArray(records))throw new Error('сервер повернув некоректний список');cardData=records;renderCards()}catch(error){status.textContent='Не вдалося завантажити список: '+error.message;console.error('Помилка завантаження списку карток:',error)}finally{listRequestInProgress=false}};window.add=async function(){const button=document.getElementById('addCardButton'),status=document.getElementById('uidAddStatus'),editing=editingCardIndex!==null,fields=['uid','lastName','firstName','middleName','vehicle','birthDate','relay'],values={};for(const field of fields)values[field]=document.getElementById(field).value;if(editing)values.index=editingCardIndex;button.disabled=true;status.textContent=editing?'Зберігаю зміни картки...':'Зберігаю картку...';try{const response=await apiRequest(editing?'/api/update':'/api/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(values)}),result=await responseMessage(response);status.textContent=result.msg||(response.ok?'Збережено':'Помилка збереження');if(!response.ok||!result.ok)return;for(const field of fields.slice(0,6))document.getElementById(field).value='';if(editing){editingCardIndex=null;document.getElementById('uid').readOnly=false;button.textContent='Додати картку';const cancelButton=document.getElementById('cancelEditButton');if(cancelButton)cancelButton.remove()}await refresh()}catch(error){status.textContent='Помилка збереження: '+error.message;console.error('Помилка збереження картки:',error)}finally{button.disabled=false}};window.onload=()=>{refresh();refreshStatus().catch(error=>console.error('Не вдалося отримати стан пристрою:',error));setInterval(()=>refreshStatus().catch(error=>console.error('Не вдалося оновити стан пристрою:',error)),5000)};</script>";
  html += "<script>window.saveCardRecord=window.add;const saveCardButton=document.getElementById('addCardButton');saveCardButton.type='button';saveCardButton.onclick=event=>{event.preventDefault();window.saveCardRecord()};</script>";
  html += "<script>const cardSaveFrame=document.createElement('iframe');cardSaveFrame.name='cardSaveFrame';cardSaveFrame.title='Відповідь сервера карток';cardSaveFrame.hidden=true;let cardSaveFrameReady=false,cardSavePending=false;cardSaveFrame.addEventListener('load',async()=>{if(!cardSaveFrameReady){cardSaveFrameReady=true;return}if(!cardSavePending)return;cardSavePending=false;const button=document.getElementById('addCardButton'),status=document.getElementById('uidAddStatus');try{const text=cardSaveFrame.contentDocument.body.textContent.trim(),result=JSON.parse(text);status.textContent=result.msg||'Сервер відповів без повідомлення';if(result.ok){for(const name of cardSaveFields.slice(0,6))document.getElementById(name).value='';if(editingCardIndex!==null){editingCardIndex=null;document.getElementById('uid').readOnly=false;button.textContent='Додати картку';const cancel=document.getElementById('cancelEditButton');if(cancel)cancel.remove()}await refresh()}}catch(error){status.textContent='Не вдалося прочитати відповідь WT32. Перевірте список карток перед повтором.';console.error('Помилка відповіді форми збереження:',error)}finally{button.disabled=false}});cardSaveFrame.src='about:blank';document.body.appendChild(cardSaveFrame);const cardSaveForm=document.createElement('form');cardSaveForm.method='POST';cardSaveForm.target=cardSaveFrame.name;cardSaveForm.hidden=true;const cardSaveFields=['uid','lastName','firstName','middleName','vehicle','birthDate','relay','index'];const cardSaveInputs={};for(const name of cardSaveFields){const input=document.createElement('input');input.type='hidden';input.name=name;cardSaveForm.appendChild(input);cardSaveInputs[name]=input}document.body.appendChild(cardSaveForm);window.saveCardRecord=function(){if(cardSavePending)return;const button=document.getElementById('addCardButton'),status=document.getElementById('uidAddStatus'),editing=editingCardIndex!==null;for(const name of cardSaveFields)cardSaveInputs[name].value=name==='index'?(editing?String(editingCardIndex):''):document.getElementById(name).value;cardSaveForm.action=editing?'/api/update':'/api/add';cardSavePending=true;button.disabled=true;status.textContent=editing?'Надсилаю зміни на WT32...':'Надсилаю картку на WT32...';cardSaveForm.submit()};window.add=window.saveCardRecord;saveCardButton.onclick=event=>{event.preventDefault();window.saveCardRecord()};</script>";
  html += "<script>const nativeCardSaveSubmit=cardSaveForm.submit.bind(cardSaveForm);cardSaveForm.submit=function(){if(!cardSaveFrameReady){cardSavePending=false;const button=document.getElementById('addCardButton');button.disabled=false;document.getElementById('uidAddStatus').textContent='Зачекайте, поки підготується з’єднання з пристроєм, і повторіть спробу.';return}this.action+=(this.action.includes('?')?'&':'?')+'iframe=1';nativeCardSaveSubmit()};</script>";
  html += "<script>let searchLogTimer;const renderCardsForSearchLog=renderCards;renderCards=function(){renderCardsForSearchLog();clearTimeout(searchLogTimer);const query=document.getElementById('cardSearch').value,matches=document.querySelectorAll('#list tr').length;searchLogTimer=setTimeout(async()=>{try{const response=await apiRequest('/api/search-log',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({query,matches})}),result=await responseMessage(response);if(!response.ok)throw new Error(result.msg||'HTTP '+response.status);console.info('Відповідь пошуку WT32:',result.matches,'збігів для',JSON.stringify(result.query))}catch(error){console.error('Не вдалося надіслати пошук до WT32:',error)}},350)};</script>";
  html += "<section class='card'><h3>Останні зчитування та облік</h3><p>Час Києва: <strong id='kyivClock'>очікується синхронізація з NTP</strong></p><label for='recentUidSelect'>Останні 10 зчитаних UID / номерів авто:</label><select id='recentUidSelect' style='width:100%'><option value=''>Поки немає зчитувань</option></select><div class='row'><button type='button' id='markArrivedButton'>Позначити «Прибув»</button><button type='button' id='markDepartedButton'>Позначити «Вибув»</button></div><p id='attendanceStatus' role='status'></p><label><input id='showOldestReads' type='checkbox'> Показати 10 карток, які найдовше не зчитувалися</label><ol id='oldestReadsList' hidden></ol><label><input id='showFrequentReads' type='checkbox'> Показати 10 найчастіше зчитаних UID</label><ol id='frequentReadsList' hidden></ol><div class='row'><div style='flex:1;min-width:240px'><h4>Прибули (останні 10)</h4><ol id='arrivalsList'></ol></div><div style='flex:1;min-width:240px'><h4>Вибули (останні 10)</h4><ol id='departuresList'></ol></div></div></section>";
  html += "<script>let cardListRefreshRunning=false,serverClockEpoch=0,serverClockReceivedAt=0;";
  html += "function displayActivityRows(elementId,items){let list=document.getElementById(elementId);list.replaceChildren();if(!items.length){let row=document.createElement('li');row.textContent='Записів поки немає';list.appendChild(row);return}for(const item of items){let row=document.createElement('li'),name=[item.vehicle,item.uid].filter(Boolean).join(' — '),when=item.epoch?new Date(item.epoch*1000).toLocaleString('uk-UA',{timeZone:'Europe/Kyiv'}):'час не синхронізовано';row.textContent=name+' ('+when+')';list.appendChild(row)}}";
  html += "async function refreshActivity(){let response=await apiRequest('/api/activity');if(!response.ok)throw new Error('HTTP '+response.status);let activity=await response.json(),select=document.getElementById('recentUidSelect'),selected=select.value;select.replaceChildren();if(!activity.recent.length){let option=document.createElement('option');option.value='';option.textContent='Поки немає зчитувань';select.appendChild(option)}else{for(const item of activity.recent){let option=document.createElement('option');option.value=item.uid;option.textContent=[item.vehicle,item.uid,item.lastName].filter(Boolean).join(' — ');select.appendChild(option)}if(activity.recent.some(item=>item.uid===selected))select.value=selected}serverClockEpoch=activity.epoch||0;serverClockReceivedAt=Date.now();displayActivityRows('arrivalsList',activity.arrivals);displayActivityRows('departuresList',activity.departures);renderReadRankings()}";
  html += "function renderReadRankings(){let oldest=cardData.slice().sort((a,b)=>Number(a.lastReadEpoch||0)-Number(b.lastReadEpoch||0)),frequent=cardData.filter(item=>Number(item.readCount)>0).sort((a,b)=>Number(b.readCount)-Number(a.readCount)||Number(b.lastReadEpoch||0)-Number(a.lastReadEpoch||0));function fill(id,items,withCount){let list=document.getElementById(id);list.replaceChildren();for(const item of items.slice(0,10)){let row=document.createElement('li'),when=Number(item.readCount)===0?'ніколи не зчитувалась':item.lastReadEpoch?new Date(item.lastReadEpoch*1000).toLocaleString('uk-UA',{timeZone:'Europe/Kyiv'}):'час не синхронізовано';row.textContent=[item.vehicle,item.uid].filter(Boolean).join(' — ')+(withCount?' — зчитувань: '+item.readCount:' — останнє зчитування: '+when);list.appendChild(row)}if(!items.length){let row=document.createElement('li');row.textContent='Статистика зчитувань ще відсутня';list.appendChild(row)}}fill('oldestReadsList',oldest,false);fill('frequentReadsList',frequent,true)}";
  html += "async function recordAttendance(action){let button=action==='arrive'?document.getElementById('markArrivedButton'):document.getElementById('markDepartedButton'),status=document.getElementById('attendanceStatus'),uid=document.getElementById('recentUidSelect').value;if(!uid){status.textContent='Спочатку зчитайте UID.';return}button.disabled=true;try{let response=await apiRequest('/api/attendance',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({uid:uid,action:action})}),result=await responseMessage(response);status.textContent=result.msg||'Подію записано';if(response.ok&&result.ok)await refreshActivity()}catch(error){status.textContent='Не вдалося записати подію: '+error.message}finally{button.disabled=false}}";
  html += "function updateKyivClock(){let clock=document.getElementById('kyivClock');if(!serverClockEpoch){clock.textContent='очікується синхронізація з NTP';return}let epoch=serverClockEpoch+Math.floor((Date.now()-serverClockReceivedAt)/1000);clock.textContent=new Date(epoch*1000).toLocaleString('uk-UA',{timeZone:'Europe/Kyiv'})}";
  html += "document.getElementById('showOldestReads').onchange=event=>document.getElementById('oldestReadsList').hidden=!event.target.checked;document.getElementById('showFrequentReads').onchange=event=>document.getElementById('frequentReadsList').hidden=!event.target.checked;document.getElementById('markArrivedButton').onclick=()=>recordAttendance('arrive');document.getElementById('markDepartedButton').onclick=()=>recordAttendance('depart');";
  html += "window.refresh=async function(){if(cardListRefreshRunning)return;cardListRefreshRunning=true;let status=document.getElementById('cardListStatus');try{let response=await apiRequest('/api/list'),body=await response.text(),records;try{records=JSON.parse(body)}catch(error){throw new Error('сервер повернув некоректну відповідь (HTTP '+response.status+')')}if(!response.ok)throw new Error(records.msg||'HTTP '+response.status);if(!Array.isArray(records))throw new Error('сервер повернув не список карток');cardData=records;renderCards();renderReadRankings()}catch(error){let message=error.name==='AbortError'?'час очікування відповіді вичерпано':error.message;status.textContent='Не вдалося завантажити список: '+message;console.error('Помилка завантаження списку карток:',error)}finally{cardListRefreshRunning=false}};";
  html += "window.onload=()=>{window.refresh();refreshStatus().catch(error=>console.error('Не вдалося оновити стан пристрою:',error));refreshActivity().catch(error=>{document.getElementById('attendanceStatus').textContent='Не вдалося завантажити історію: '+error.message});setInterval(()=>window.refresh(),10000);setInterval(()=>refreshStatus().catch(error=>console.error('Не вдалося оновити стан пристрою:',error)),5000);setInterval(()=>refreshActivity().catch(error=>console.error('Не вдалося оновити активність:',error)),15000);setInterval(updateKyivClock,1000)};</script>";
  html += "<section class='card'><h3>Мережа</h3><p><a href='/wifi' onclick='openWifi(event)'>Налаштувати WiFi для резервного підключення</a></p><p><a href='/mqtt'>Налаштувати MQTT-брокер</a></p><p>Якщо роутер недоступний, підключіться до WT32-Config і відкрийте 192.168.4.1.</p></section></body></html>";
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "text/html", html);
}

void handleRoot() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (!LittleFS.exists("/index.html")) {
    Serial.println("HTTP /: /index.html відсутній у LittleFS; завантажте web-файли командою uploadfs.");
    server.send(503, "text/plain; charset=utf-8",
                "Web files are missing. Upload the LittleFS image with PlatformIO target uploadfs.");
    return;
  }
  AsyncWebServerResponse *response =
      server.request->beginResponse(LittleFS, "/index.html", "text/html");
  if (!response) {
    Serial.println("HTTP /: не вдалося створити відповідь для /index.html.");
    server.send(503, "text/plain; charset=utf-8",
                "The web page could not be prepared. Please retry the request.");
    return;
  }
  response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  server.request->send(response);
}

void handleListAPI(AsyncWebServerRequest *request) {
  if (!request->authenticate(webUser, webPass)) {
    request->requestAuthentication();
    return;
  }
  if (!cardStorageReady) {
    Serial.println("API /api/list: сховище карток недоступне.");
    request->send(503, "application/json",
                  "{\"ok\":false,\"msg\":\"Сховище карток недоступне\"}");
    return;
  }
  String json = getListJSON();
  Serial.print("API /api/list: повертаю карток: ");
  Serial.print(cardCount);
  Serial.print(", JSON bytes: ");
  Serial.println(json.length());
  AsyncWebServerResponse *response =
      request->beginResponse(200, "application/json; charset=utf-8", json);
  if (!response) {
    Serial.println("API /api/list: не вдалося створити HTTP-відповідь.");
    request->send(500, "application/json",
                  "{\"ok\":false,\"msg\":\"Не вдалося підготувати список карток\"}");
    return;
  }
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void handleSearchLogAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  String query = server.arg("query");
  long matches = server.arg("matches").toInt();
  if (matches < 0) matches = 0;
  Serial.print("Пошук: запит=\"");
  Serial.print(query.length() ? query : "(порожній)");
  Serial.println("\"");
  Serial.print("Пошук: відповідь інтерфейсу — збігів: ");
  Serial.println(matches);
  Serial.println("Пошук: журнал запиту підтверджено пристроєм.");

  JsonDocument doc;
  doc["ok"] = true;
  doc["query"] = query;
  doc["matches"] = matches;
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleActivityAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.send(200, "application/json", getActivityJSON());
}

void handleAttendanceAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  String uid = normalizeUIDInput(server.arg("uid"));
  String action = server.arg("action");
  if (!isValidUIDFormat(uid) || (action != "arrive" && action != "depart")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"msg\":\"Виберіть UID і прибуття або вибуття\"}");
    return;
  }
  if (attendanceEventCount >= MAX_ATTENDANCE_EVENTS_PER_TYPE * 2) {
    size_t oldestIndex = attendanceEventCount;
    for (size_t i = 0; i < attendanceEventCount; i++) {
      if (attendanceEvents[i].arrived == (action == "arrive")) {
        oldestIndex = i;
        break;
      }
    }
    if (oldestIndex == attendanceEventCount) oldestIndex = 0;
    for (size_t i = oldestIndex; i + 1 < attendanceEventCount; i++) {
      attendanceEvents[i] = attendanceEvents[i + 1];
    }
    attendanceEventCount--;
  } else {
    size_t typeCount = 0;
    for (size_t i = 0; i < attendanceEventCount; i++) {
      if (attendanceEvents[i].arrived == (action == "arrive")) typeCount++;
    }
    if (typeCount >= MAX_ATTENDANCE_EVENTS_PER_TYPE) {
      for (size_t i = 0; i < attendanceEventCount; i++) {
        if (attendanceEvents[i].arrived == (action == "arrive")) {
          for (size_t j = i; j + 1 < attendanceEventCount; j++) {
            attendanceEvents[j] = attendanceEvents[j + 1];
          }
          attendanceEventCount--;
          break;
        }
      }
    }
  }

  AttendanceEvent &event = attendanceEvents[attendanceEventCount++];
  memset(&event, 0, sizeof(event));
  copyCardField(event.uid, sizeof(event.uid), uid);
  time_t now;
  time(&now);
  setAttendanceEventEpoch(event, now >= 1704067200 ? (uint32_t)now : 0);
  event.arrived = action == "arrive";
  if (!saveAttendanceEvents()) {
    initializeAttendanceStorage();
    server.send(500, "application/json",
                "{\"ok\":false,\"msg\":\"Не вдалося зберегти журнал у SPIFFS\"}");
    return;
  }

  Serial.print("Журнал: UID ");
  Serial.print(uid);
  Serial.println(event.arrived ? " позначено як прибув." : " позначено як вибув.");
  server.send(200, "application/json",
              event.arrived
                  ? "{\"ok\":true,\"msg\":\"Прибуття записано\"}"
                  : "{\"ok\":true,\"msg\":\"Вибуття записано\"}");
}

void handleStatusAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  String network = (currentNet == NET_ETH) ? "Ethernet" : (currentNet == NET_WIFI_STA) ? "WiFi" : (currentNet == NET_WIFI_AP) ? "AP" : "offline";
  String ip = (currentNet == NET_ETH) ? ETH.localIP().toString() : (currentNet == NET_WIFI_STA) ? WiFi.localIP().toString() : (currentNet == NET_WIFI_AP) ? WiFi.softAPIP().toString() : "";
  String json = "{\"uid\":\"" + lastScannedUID + "\",\"pn532\":" + String(nfcPresent ? "true" : "false") + ",\"network\":\"" + network + "\",\"ip\":\"" + ip + "\"}";
  server.send(200, "application/json", json);
}

void sendCardAPIResponse(int statusCode, const String &body) {
  const char *contentType = server.hasArg("iframe")
      ? "text/plain; charset=utf-8"
      : "application/json";
  server.send(statusCode, contentType, body);
}

void handleAddAPI() {
  Serial.println("HTTP POST /api/add отримано.");
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (!cardStorageReady) {
    Serial.println("API /api/add: сховище карток недоступне.");
    sendCardAPIResponse(503, "{\"ok\":false,\"msg\":\"Сховище карток недоступне\"}");
    return;
  }
  Serial.println("API /api/add: отримано запит на додавання картки.");
  String uid = server.arg("uid");
  uid = normalizeUIDInput(uid);
  if (!isValidUIDFormat(uid)) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Невірний формат UID\"}");
    return;
  }
  if (cardUIDExists(uid)) {
    sendCardAPIResponse(409, "{\"ok\":false,\"msg\":\"Цей UID вже є у списку\"}");
    return;
  }
  if (cardCount >= MAX_CARDS) {
    sendCardAPIResponse(507, "{\"ok\":false,\"msg\":\"Список заповнений: максимальна кількість карток — 300\"}");
    return;
  }

  String lastName = server.arg("lastName");
  String firstName = server.arg("firstName");
  String middleName = server.arg("middleName");
  String vehicle = server.arg("vehicle");
  String birthDate = server.arg("birthDate");
  if (lastName.length() >= NAME_FIELD_SIZE ||
      firstName.length() >= NAME_FIELD_SIZE ||
      middleName.length() >= NAME_FIELD_SIZE ||
      vehicle.length() >= VEHICLE_FIELD_SIZE ||
      birthDate.length() >= BIRTH_DATE_FIELD_SIZE) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Перевищено допустиму довжину одного з полів\"}");
    return;
  }
  if (!isValidBirthDate(birthDate)) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Перевірте дату народження (РРРР-ММ-ДД)\"}");
    return;
  }

  int r = server.arg("relay").toInt();
  if (r < 1 || r > 3) r = 1;
  bool ok = addRecord(uid, (uint8_t)r, lastName, firstName, middleName, vehicle, birthDate);
  if (ok) {
    Serial.print("API /api/add: картку збережено, всього: ");
    Serial.println(cardCount);
    Serial.print("Збережені поля: UID=\""); Serial.print(uid);
    Serial.print("\"; прізвище=\""); Serial.print(lastName);
    Serial.print("\"; ім’я=\""); Serial.print(firstName);
    Serial.print("\"; по батькові=\""); Serial.print(middleName);
    Serial.print("\"; авто=\""); Serial.print(vehicle);
    Serial.print("\"; дата народження=\""); Serial.print(birthDate);
    Serial.print("\"; реле="); Serial.println(r);
    sendCardAPIResponse(200, "{\"ok\":true,\"msg\":\"Картку збережено\"}");
  }
  else {
    Serial.println("API /api/add: addRecord не зміг записати картку.");
    sendCardAPIResponse(500, "{\"ok\":false,\"msg\":\"Не вдалося зберегти картку у пам’яті пристрою\"}");
  }
}

void handleUpdateCardAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (!cardStorageReady) {
    sendCardAPIResponse(503, "{\"ok\":false,\"msg\":\"Сховище карток недоступне\"}");
    return;
  }
  int index = server.arg("index").toInt();
  if (index < 0 || (size_t)index >= cardCount) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Невірний індекс картки\"}");
    return;
  }

  String lastName = server.arg("lastName");
  String firstName = server.arg("firstName");
  String middleName = server.arg("middleName");
  String vehicle = server.arg("vehicle");
  String birthDate = server.arg("birthDate");
  if (lastName.length() >= NAME_FIELD_SIZE ||
      firstName.length() >= NAME_FIELD_SIZE ||
      middleName.length() >= NAME_FIELD_SIZE ||
      vehicle.length() >= VEHICLE_FIELD_SIZE ||
      birthDate.length() >= BIRTH_DATE_FIELD_SIZE) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Перевищено допустиму довжину одного з полів\"}");
    return;
  }
  if (!isValidBirthDate(birthDate)) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Перевірте дату народження (РРРР-ММ-ДД)\"}");
    return;
  }

  int relay = server.arg("relay").toInt();
  if (relay < 1 || relay > 3) {
    sendCardAPIResponse(400, "{\"ok\":false,\"msg\":\"Некоректне призначення реле\"}");
    return;
  }
  if (updateCardRecord((size_t)index, (uint8_t)relay, lastName, firstName,
                       middleName, vehicle, birthDate)) {
    Serial.print("API /api/update: картку оновлено, індекс: ");
    Serial.println(index);
    Serial.print("Збережені поля: UID=\""); Serial.print(cardRecords[index].uid);
    Serial.print("\"; прізвище=\""); Serial.print(lastName);
    Serial.print("\"; ім’я=\""); Serial.print(firstName);
    Serial.print("\"; по батькові=\""); Serial.print(middleName);
    Serial.print("\"; авто=\""); Serial.print(vehicle);
    Serial.print("\"; дата народження=\""); Serial.print(birthDate);
    Serial.print("\"; реле="); Serial.println(relay);
    sendCardAPIResponse(200, "{\"ok\":true,\"msg\":\"Дані картки оновлено\"}");
  } else {
    sendCardAPIResponse(500, "{\"ok\":false,\"msg\":\"Не вдалося зберегти зміни картки\"}");
  }
}

void handleAddLastAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (!cardStorageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"msg\":\"Сховище карток недоступне\"}");
    return;
  }
  if (lastScannedUID.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Ще не зчитано жодної картки\"}");
    return;
  }
  int relay = server.arg("relay").toInt();
  if (relay < 1 || relay > 3) relay = 1;
  bool ok = addRecord(lastScannedUID, (uint8_t)relay);
  server.send(200, "application/json", ok
    ? "{\"ok\":true,\"msg\":\"Картку додано\"}"
    : "{\"ok\":false,\"msg\":\"Не додано: дублікат або список заповнений\"}");
}

void handleRelayAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  int mask = server.arg("mask").toInt();
  if (mask < 1 || mask > 3) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Некоректний номер реле\"}");
    return;
  }
  String action = server.arg("action");
  if (action == "stop") {
    stopRelays((uint8_t)mask);
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Реле вимкнено\"}");
    return;
  }
  if (action != "pulse") {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Некоректна команда\"}");
    return;
  }
  int duration = server.arg("duration").toInt();
  if (duration == 0) duration = clickDuration;
  duration = constrain(duration, 50, 60000);
  pulseRelays((uint8_t)mask, (uint32_t)duration);
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Реле увімкнено на " + String(duration) + " мс\"}");
}

void handleSettingsAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  int duration = server.arg("duration").toInt();
  if (duration < 50 || duration > 60000) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Тривалість має бути від 50 до 60000 мс\"}");
    return;
  }
  clickDuration = duration;
  EEPROM.put(ADDR_CLICK_DURATION, clickDuration);
  EEPROM.commit();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Тривалість імпульсу збережено\"}");
}

void handleRemoveAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (!cardStorageReady) {
    server.send(503, "application/json", "{\"ok\":false,\"msg\":\"Сховище карток недоступне\"}");
    return;
  }
  String idxs = server.arg("index");
  int idx = idxs.toInt();
  if (idx < 0 || (size_t)idx >= cardCount) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Невірний індекс\"}");
    return;
  }
  if (removeCardRecord((size_t)idx)) {
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Видалено\"}");
  } else {
    server.send(500, "application/json", "{\"ok\":false,\"msg\":\"Не вдалося зберегти зміни списку\"}");
  }
}

void handleWiFiScan() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }

  auto sendScanResults = [](int16_t scanResult) {
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < scanResult; i++) {
      JsonObject network = networks.add<JsonObject>();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
      network["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    serializeJson(doc, wifiScanResultJson);
    WiFi.scanDelete();
    wifiScanInProgress = false;
    server.send(200, "application/json", wifiScanResultJson);
  };

  if (wifiScanInProgress) {
    int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
      server.send(200, "application/json", "{\"scanning\":true}");
      return;
    }
    if (result >= 0) {
      sendScanResults(result);
      return;
    }
    wifiScanInProgress = false;
    WiFi.scanDelete();
    server.send(500, "application/json", "{\"error\":\"Пошук WiFi завершився помилкою. Натисніть пошук ще раз.\"}");
    return;
  }

  if (wifiScanResultJson.length() > 0 && server.arg("start") != "1") {
    server.send(200, "application/json", wifiScanResultJson);
    return;
  }
  if (server.arg("start") != "1") {
    server.send(200, "application/json", "{\"networks\":[]}");
    return;
  }

  wifiScanResultJson = "";
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) WiFi.mode(WIFI_STA);
  else if (mode == WIFI_AP) WiFi.mode(WIFI_AP_STA);
  WiFi.scanDelete();

  int16_t started = WiFi.scanNetworks(true, true);
  if (started == WIFI_SCAN_RUNNING) {
    wifiScanInProgress = true;
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (started >= 0) {
    sendScanResults(started);
    return;
  }
  server.send(500, "application/json", "{\"error\":\"Не вдалося запустити пошук WiFi мереж\"}");
}

String htmlEscape(const String &value);

// WiFi конфіг сторінка
void handleWiFiSaveLegacy() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (server.method() == HTTP_POST) {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    if (ssid.length() == 0) {
      server.send(400, "text/plain", "SSID порожній");
      return;
    }
    if (ssid.length() > 32 || pass.length() > 63) {
      server.send(400, "text/plain", "SSID має бути до 32 символів, пароль — до 63");
      return;
    }
    if (pass.length() == 0 && ssid == readWiFiSSID()) {
      pass = readWiFiPass();
    }
    writeWiFiCreds(ssid, pass);
    wifiConnectPending = true;
    wifiConnectPendingAt = millis() + 500;
    server.send(200, "text/html",
      "<!doctype html><html lang='uk'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<h3>Параметри WiFi збережено</h3><p>Пристрій спробує підключитися автоматично. Якщо Ethernet підключений, він залишиться основним.</p>"
      "<p>Якщо підключення успішне, відкрийте пристрій за новою IP-адресою у вашій мережі.</p><p><a href='/wifi'>Повернутися до налаштувань</a> | <a href='/'>Головна</a></p></html>");
    return;
  }

  String ssid = readWiFiSSID();
  String html = "<!doctype html><html lang='uk'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Налаштування WiFi</title><style>body{font:16px Arial,sans-serif;margin:16px;background:#f3f5f7;color:#202124}"
    "main{background:#fff;padding:18px;border-radius:10px;margin:auto;max-width:600px}button,input,select{font:inherit;padding:9px;margin:6px 0}"
    "input,select{box-sizing:border-box;width:100%}button{cursor:pointer}</style></head><body><main><h2>WiFi для резервного підключення</h2>"
    "<p>Знайдіть мережу, виберіть її, введіть пароль і натисніть «Застосувати». Ethernet залишиться основним, якщо він підключений.</p>"
    "<button type='button' id='scan'>Знайти WiFi мережі</button><p id='scanStatus' role='status'></p>"
    "<label for='networks'>Видимі мережі:</label><select id='networks'><option value=''>Спочатку виконайте пошук</option></select>"
    "<form method='POST' action='/wifi'><label for='ssid'>Назва мережі (SSID):</label>"
    "<input id='ssid' name='ssid' maxlength='32' required value='" + htmlEscape(ssid) + "'>"
    "<label for='pass'>Пароль WiFi:</label><input id='pass' name='pass' maxlength='63' type='password' autocomplete='new-password'>"
    "<p>Для відкритої мережі залиште пароль порожнім. Для тієї самої мережі порожнє поле зберігає попередній пароль.</p>"
    "<button type='submit'>Застосувати</button></form><p><a href='/'>Назад</a></p></main>"
    "<script>const select=document.getElementById('networks'),ssid=document.getElementById('ssid'),status=document.getElementById('scanStatus');"
    "select.onchange=()=>{if(select.value)ssid.value=select.value};"
    "async function scan(){document.getElementById('scan').disabled=true;status.textContent='Шукаю мережі...';"
    "try{let data;do{let url='/wifi/scan'+(data?'':'?start=1');let r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error('Помилка пошуку мереж');data=await r.json();"
    "if(data.scanning)await new Promise(resolve=>setTimeout(resolve,700))}while(data.scanning);"
    "if(data.error)throw new Error(data.error);select.replaceChildren();select.add(new Option('Оберіть мережу',''));for(const n of data.networks){const o=document.createElement('option');o.value=n.ssid;"
    "o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.open?' — відкрита':' — пароль');select.appendChild(o)}"
    "if(!data.networks.length){status.textContent='Мереж не знайдено. Перевірте, що роутер працює поруч.'}"
    "else{status.textContent='Знайдено мереж: '+data.networks.length+'. Виберіть потрібну зі списку.'}}"
    "catch(e){status.textContent=e.message}finally{document.getElementById('scan').disabled=false}}"
    "document.getElementById('scan').onclick=scan;scan();</script></html>";
  server.send(200, "text/html", html);
}

String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') escaped += "&amp;";
    else if (c == '"') escaped += "&quot;";
    else if (c == '<') escaped += "&lt;";
    else if (c == '>') escaped += "&gt;";
    else escaped += c;
  }
  return escaped;
}

void handleMqttSettings() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }

  if (server.method() == HTTP_POST) {
    String host = server.arg("host");
    String portText = server.arg("port");
    String user = server.arg("user");
    String pass = server.arg("pass");
    String topic = server.arg("topic");
    host.trim();
    user.trim();
    topic.trim();
    while (topic.endsWith("/")) topic.remove(topic.length() - 1);

    long portValue = portText.toInt();
    bool invalidTopic = topic.length() == 0 || topic.indexOf('#') >= 0 ||
                        topic.indexOf('+') >= 0 || topic.indexOf(' ') >= 0;
    if (host.length() > MQTT_MAX_HOST - 1 || host.indexOf(' ') >= 0 ||
        portValue < 1 || portValue > 65535 ||
        user.length() > MQTT_MAX_USER - 1 || pass.length() > MQTT_MAX_PASS - 1 ||
        topic.length() > MQTT_MAX_TOPIC - 1 || invalidTopic) {
      server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Перевірте адресу брокера, порт, логін, пароль і базовий MQTT topic\"}");
      return;
    }
    if (pass.length() == 0) pass = readMqttPass();

    eepromWriteString(ADDR_MQTT_HOST, host, MQTT_MAX_HOST);
    EEPROM.put(ADDR_MQTT_PORT, (uint16_t)portValue);
    eepromWriteString(ADDR_MQTT_USER, user, MQTT_MAX_USER);
    eepromWriteString(ADDR_MQTT_PASS, pass, MQTT_MAX_PASS);
    eepromWriteString(ADDR_MQTT_TOPIC, topic, MQTT_MAX_TOPIC);
    EEPROM.commit();
    mqttClient.disconnect();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"MQTT налаштування збережено. Підключення буде повторено автоматично.\"}");
    return;
  }

  server.request->send(LittleFS, "/mqtt.html", "text/html");
}

void handleMqttConfigAPI() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  JsonDocument doc;
  doc["host"] = readMqttHost();
  doc["port"] = readMqttPort();
  doc["user"] = readMqttUser();
  doc["topic"] = readMqttTopic();
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleStaticWiFiPage() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/wifi.html", "text/html");
}

void handleWiFiJS() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/wifi.js", "application/javascript");
}

void handleMqttJS() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/mqtt.js", "application/javascript");
}

void handleStyleCSS() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/style.css", "text/css");
}

void handleAppJS() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/app.js", "application/javascript");
}

void handleRootPage() {
  handleRoot();
}

void handleWiFiPage() {
  handleStaticWiFiPage();
}

void handleMqttPage() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/mqtt.html", "text/html");
}

void handleWiFiSave() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  if (server.method() != HTTP_POST) {
    server.request->send(LittleFS, "/wifi.html", "text/html");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"SSID порожній\"}");
    return;
  }
  if (ssid.length() > 32 || pass.length() > 63) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"SSID має бути до 32 символів, пароль — до 63\"}");
    return;
  }
  if (pass.length() == 0 && ssid == readWiFiSSID()) pass = readWiFiPass();
  writeWiFiCreds(ssid, pass);
  wifiConnectPending = true;
  wifiConnectPendingAt = millis() + 500;
  server.send(200, "application/json",
      "{\"ok\":true,\"msg\":\"Параметри WiFi збережено. Пристрій спробує підключитися автоматично.\"}");
}

void handleIndexPage() {
  handleRoot();
}

void handleLegacyMqttSettings() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }
  server.request->send(LittleFS, "/mqtt.html", "text/html");
}

/* Legacy page implementation retained below only for source-history context. */
void handleMqttSettingsLegacy() {
  if (!server.authenticate(webUser, webPass)) { server.requestAuthentication(); return; }

  String html = "<!doctype html><html lang='uk'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>MQTT</title><h3>Налаштування MQTT</h3><form method='POST' action='/mqtt'>";
  html += "<label>IP або ім’я хоста брокера:</label><input name='host' maxlength='64' value='" + htmlEscape(readMqttHost()) + "'>";
  html += "<label>Порт:</label><input name='port' type='number' min='1' max='65535' value='" + String(readMqttPort()) + "'>";
  html += "<label>Логін (необов’язково):</label><input name='user' maxlength='32' value='" + htmlEscape(readMqttUser()) + "'>";
  html += "<label>Пароль (порожнє поле залишає поточний):</label><input name='pass' maxlength='64' type='password'>";
  html += "<label>Базовий topic:</label><input name='topic' maxlength='64' value='" + htmlEscape(readMqttTopic()) + "'>";
  html += "<button type='submit'>Зберегти</button></form><p>Порожня адреса брокера вимикає MQTT. Типовий порт — 1883.</p><p><a href='/'>Назад</a></p></html>";
  server.send(200, "text/html", html);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  EEPROM.begin(EEPROM_SIZE);
  cardRecords = static_cast<CardRecord *>(calloc(MAX_CARDS, sizeof(CardRecord)));
  if (!cardRecords) {
    Serial.println("Не вдалося виділити пам’ять для реєстру карток.");
    cardStorageReady = false;
  } else {
    cardStorageReady = initializeCardStorage();
  }
  if (cardStorageReady) seedRecentScans();
  if (cardStorageReady && !initializeAttendanceStorage()) {
    Serial.println("Журнал прибуття/вибуття недоступний; нові події не буде втрачено без повідомлення.");
  }
  lastCardStatsSave = millis();

  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  digitalWrite(RELAY_1, LOW);
  digitalWrite(RELAY_2, LOW);

  // GPIO39 є input-only і не має внутрішнього підтягування. Потрібен 10 кОм до 3.3 V;
  // кнопку підключити між GPIO39 та GND.
  pinMode(BUTTON_ADD, INPUT);

  // I2C та PN532
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(30);

  // Зчитати clickDuration (якщо зберігали раніше)
  int savedDuration = 0;
  EEPROM.get(ADDR_CLICK_DURATION, savedDuration);
  if (savedDuration >= 50 && savedDuration <= 60000) clickDuration = savedDuration;

  // Ініціалізація Ethernet
  Serial.println("Запускаю Ethernet; NFC і реле працюють незалежно від мережі.");
  bool ethStarted = ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
  if (!ethStarted) Serial.println("Не вдалося запустити Ethernet PHY.");
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();
  networkPhaseStarted = millis();

  // Веб-роути
  asyncServer.addHandler(&webSocket);
  webSocket.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *, size_t) {
    if (type != WS_EVT_CONNECT) return;
    AsyncWebServerRequest *request = static_cast<AsyncWebServerRequest *>(arg);
    if (!request || !request->authenticate(webUser, webPass)) {
      client->close(1008, "Authentication required");
      return;
    }
    client->text(buildLiveStatusJSON("status"));
  });
  server.on("/", HTTP_GET, handleRoot);
  asyncServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(webUser, webPass)) { request->requestAuthentication(); return; }
    request->send(LittleFS, "/style.css", "text/css");
  });
  asyncServer.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(webUser, webPass)) { request->requestAuthentication(); return; }
    request->send(LittleFS, "/app.js", "application/javascript");
  });
  asyncServer.on("/wifi.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(webUser, webPass)) { request->requestAuthentication(); return; }
    request->send(LittleFS, "/wifi.js", "application/javascript");
  });
  asyncServer.on("/mqtt.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(webUser, webPass)) { request->requestAuthentication(); return; }
    request->send(LittleFS, "/mqtt.js", "application/javascript");
  });
  asyncServer.on("/api/list", HTTP_GET, handleListAPI);
  server.on("/api/search-log", HTTP_POST, handleSearchLogAPI);
  server.on("/api/activity", HTTP_GET, handleActivityAPI);
  server.on("/api/attendance", HTTP_POST, handleAttendanceAPI);
  server.on("/api/status", HTTP_GET, handleStatusAPI);
  server.on("/api/mqtt-config", HTTP_GET, handleMqttConfigAPI);
  server.on("/api/add", HTTP_POST, handleAddAPI);
  server.on("/api/update", HTTP_POST, handleUpdateCardAPI);
  server.on("/api/add-last", HTTP_POST, handleAddLastAPI);
  server.on("/api/remove", HTTP_POST, handleRemoveAPI);
  server.on("/api/relay", HTTP_POST, handleRelayAPI);
  server.on("/api/settings", HTTP_POST, handleSettingsAPI);
  server.on("/wifi/scan", HTTP_GET, handleWiFiScan);
  server.on("/wifi", HTTP_GET, handleWiFiSave);
  server.on("/wifi", HTTP_POST, handleWiFiSave);
  server.on("/mqtt", HTTP_GET, handleMqttSettings);
  server.on("/mqtt", HTTP_POST, handleMqttSettings);
  server.onNotFound(handleNotFound);
  server.begin();
  mqttClient.setBufferSize(512);
  Serial.println("Веб-сервер запущено на порті 80");
  Serial.println("Очікую IP Ethernet; якщо зв'язку не буде — підключу WiFi або локальну AP.");

  // Network and web startup must not wait for a PN532 I2C response.
  lastNfcInitAttempt = millis() - NFC_RETRY_INTERVAL;
  lastNfcFailureLog = millis() - 30000;
}

// -------------------- Loop --------------------
void loop() {
  if (captiveDnsActive) dnsServer.processNextRequest();

  unsigned long now = millis();

  serviceRelays(now);
  serviceNetwork(now);
  ensureTimeSync();
  serviceMqtt(now);
  if (now - lastWebSocketBroadcast >= 1000) {
    lastWebSocketBroadcast = now;
    if (webSocket.count() > 0) {
      webSocket.textAll(buildLiveStatusJSON("status"));
    }
    webSocket.cleanupClients();
  }
  if (cardStatsDirty && now - lastCardStatsSave >= CARD_STATS_SAVE_INTERVAL) {
    lastCardStatsSave = now;
    if (saveCardRecords()) {
      cardStatsDirty = false;
    } else {
      Serial.println("Статистика зчитувань залишилася незбереженою; буде повторна спроба.");
    }
  }

  if (!nfcPresent && now - lastNfcInitAttempt >= NFC_RETRY_INTERVAL) {
    lastNfcInitAttempt = now;
    if (!initializePN532() && now - lastNfcFailureLog >= 30000) {
      Serial.println("PN532 поки не знайдено; перевірте режим I2C, живлення та SDA/SCL; повторні спроби тривають.");
      lastNfcFailureLog = now;
    }
  }

  // Hardware button (GPIO39 needs an external pull-up). Debounce its input
  // before treating a low level as a press.
  static bool rawButtonState = false;
  static bool stableButtonState = false;
  static unsigned long buttonStateChangedAt = 0;
  static unsigned long lastMissingUIDMessage = 0;
  static bool loggedMissingUID = false;
  const unsigned long BUTTON_DEBOUNCE_MS = 50;
  bool pressed = (digitalRead(BUTTON_ADD) == LOW);
  if (pressed != rawButtonState) {
    rawButtonState = pressed;
    buttonStateChangedAt = now;
  }
  if (rawButtonState != stableButtonState && now - buttonStateChangedAt >= BUTTON_DEBOUNCE_MS) {
    stableButtonState = rawButtonState;
    if (stableButtonState) {
      if (lastScannedUID.length() > 0) {
        bool ok = addRecord(lastScannedUID, 1);
        Serial.println(ok ? "Додано через кнопку" : "Не додано через кнопку (дубль/нема місця)");
      } else if (!loggedMissingUID || now - lastMissingUIDMessage >= 10000) {
        Serial.println("Немає останнього UID для додавання");
        lastMissingUIDMessage = now;
        loggedMissingUID = true;
      }
    }
  }

  // NFC опитування
  if (nfcPresent && (now - lastNFCRead >= NFC_POLL_INTERVAL)) {
    lastNFCRead = now;
    uint8_t uid[7];
    uint8_t uidLength = 0;
    bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 20);
    if (success && uidLength > 0) {
      String currentUID = formatUID(uid, uidLength);
      lastScannedUID = currentUID;
      lastCardSeen = now;
      if (currentUID != presentedUID) {
        presentedUID = currentUID;
        Serial.print("Зчитано UID: "); Serial.println(currentUID);
        trackCardRead(currentUID);
        if (webSocket.count() > 0) {
          webSocket.textAll(buildLiveStatusJSON("scan"));
        }
        uint8_t mask = findUIDRelayMask(currentUID);
        publishMqttCard(currentUID, mask);
        if (mask != 0) {
          pulseRelays(mask, (uint32_t)clickDuration);
          Serial.print("Дозволена картка, реле: "); Serial.println(mask);
        } else {
          Serial.println("UID відсутній у списку");
        }
      }
    } else if (presentedUID.length() > 0 && now - lastCardSeen >= CARD_REARM_MS) {
      presentedUID = ""; // повторне прикладання тієї ж картки знову активує реле
    }
  }

  delay(2);
}
