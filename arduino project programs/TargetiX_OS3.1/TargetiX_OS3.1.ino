#include <WiFi.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>

DNSServer dnsServer;
const byte DNS_PORT = 53;

#define FW_VERSION "3.1"
#define EEPROM_WELCOME_ADDR 3510

const char* versionURL =
  "https://raw.githubusercontent.com/ua2026-hash/TargetiX_OS/main/version.txt";

const char* firmwareURL =
  "https://raw.githubusercontent.com/ua2026-hash/TargetiX_OS/main/firmware_sl5.bin";

// ====== Wi-Fi AP ======
const char* ssid = "TargetiX_SL5";
const char* password = "sport_pistol";
WiFiServer server(80);

// ====== IO ======
const int redLedPin = 2;
const int greenLedPin = 4;
const int buttonPin = 18;

// ====== Runtime state ======
bool isProgramRunning = false;
bool stopRequested = false;
bool finishingRed = false;
unsigned long finishStartTime = 0;

unsigned long stageStartTime = 0;
int currentStage = 0;
int totalStages = 0;
unsigned long redDuration = 0;    // ms
unsigned long greenDuration = 0;  // ms
bool stageRed = true;
int activeProgramId = -1;   // ID активной программы
String lastOTAUpdate = "";  // время последнего OTA
String remoteVersion = "";
String updateChangelog = "• BatteryFix";
bool updateAvailableFlag = false;
volatile int otaProgress = 0;

// ====== Language ======
enum Language {
  LANG_DE,
  LANG_UA,
  LANG_EN
};

Language currentLang = LANG_DE;
bool setupCompleted = false;
bool welcomeCompleted = false;
bool wifiSetupCompleted = false;
bool staEnabled = true;

String savedSSID = "";
String savedPASS = "";

#define EEPROM_LANG_ADDR 3500

// ====== EEPROM: user programs ======
#define EEPROM_SIZE 2048
#define MAX_USER_PROGRAMS 12

#define EEPROM_SIZE 4096

#define MAX_CARDS 6
#define MAX_PROGRAMS_PER_CARD 8

struct UserProgram {
  char name[20];
  uint16_t redSec;
  uint16_t greenSec;
  uint16_t series;
};

struct ProgramCard {
  char title[20];
  uint8_t count;
  UserProgram programs[MAX_PROGRAMS_PER_CARD];
};

// EEPROM layout:
// [0] uint16_t signature = 0xBEEF
// [2] uint16_t count
// [4] array UserProgram[MAX_USER_PROGRAMS]
const uint16_t EEPROM_SIGNATURE = 0xBEEF;
UserProgram userPrograms[MAX_USER_PROGRAMS];
uint16_t userProgramCount = 0;
ProgramCard cards[MAX_CARDS];
uint8_t cardCount = 0;


// ====== Последняя программа для кнопки ======
uint16_t lastRed = 0, lastGreen = 0, lastSeries = 0;

// Адрес хранения последних значений в EEPROM
#define EEPROM_LAST_ADDR 2000
#define EEPROM_WIFI_FLAG 3000
#define EEPROM_WIFI_SSID 3010
#define EEPROM_WIFI_PASS 3050

void saveLastProgram() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_LAST_ADDR, lastRed);
  EEPROM.put(EEPROM_LAST_ADDR + sizeof(uint16_t), lastGreen);
  EEPROM.put(EEPROM_LAST_ADDR + 2 * sizeof(uint16_t), lastSeries);
  EEPROM.commit();
  EEPROM.end();
}

void loadLastProgram() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_LAST_ADDR, lastRed);
  EEPROM.get(EEPROM_LAST_ADDR + sizeof(uint16_t), lastGreen);
  EEPROM.get(EEPROM_LAST_ADDR + 2 * sizeof(uint16_t), lastSeries);
  EEPROM.end();

  if (lastRed > 0 && lastGreen > 0 && lastSeries > 0) {
    Serial.printf("↩️ Восстановлена последняя программа: R=%u G=%u S=%u\n", lastRed, lastGreen, lastSeries);
  } else {
    lastRed = lastGreen = lastSeries = 0;
  }
}

void eepromLoad() {
  EEPROM.begin(EEPROM_SIZE);

  uint16_t sig;
  EEPROM.get(0, sig);

  if (sig != EEPROM_SIGNATURE) {
    cardCount = 0;
    EEPROM.put(0, EEPROM_SIGNATURE);
    EEPROM.put(2, cardCount);
    EEPROM.commit();
    EEPROM.end();
    return;
  }

  EEPROM.get(2, cardCount);
  if (cardCount > MAX_CARDS) cardCount = 0;

  EEPROM.get(3, cards);
  EEPROM.end();
}


void eepromSave() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, EEPROM_SIGNATURE);
  EEPROM.put(2, cardCount);
  EEPROM.put(3, cards);
  EEPROM.commit();
  EEPROM.end();
}

bool loadWiFiCredentials(String& ssid, String& pass) {

  EEPROM.begin(EEPROM_SIZE);

  uint8_t flag;

  EEPROM.get(EEPROM_WIFI_FLAG, flag);

  if (flag != 1) {
    EEPROM.end();
    return false;
  }

  char ssidBuf[32];
  char passBuf[64];

  EEPROM.get(EEPROM_WIFI_SSID, ssidBuf);
  EEPROM.get(EEPROM_WIFI_PASS, passBuf);

  EEPROM.end();

  ssid = String(ssidBuf);
  pass = String(passBuf);

  return ssid.length() > 0;
}

bool updateAvailable() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url = String(versionURL) + "?t=" + String(millis());  // анти-кеш

  https.setTimeout(5000);
  https.setReuse(false);

  if (!https.begin(client, url)) {
    Serial.println("❌ HTTPS begin failed");
    return false;
  }

  int httpCode = https.GET();

  if (httpCode != HTTP_CODE_OK) {
    https.end();
    return false;
  }

  String newVersion = https.getString();
  newVersion.trim();

  https.end();

  Serial.println("Текущая версия: " + String(FW_VERSION));
  Serial.println("GitHub версия: " + newVersion);

  return newVersion != String(FW_VERSION);
}

void performOTAUpdate() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  Serial.println("🔄 Проверка обновления...");

  otaProgress = 0;

  httpUpdate.onProgress([](int cur, int total) {
    otaProgress = (cur * 100) / total;

    Serial.printf(
      "📦 OTA Progress: %d%% (%d / %d)\n",
      otaProgress,
      cur,
      total);
  });

  t_httpUpdate_return result =
    httpUpdate.update(client, firmwareURL);

  switch (result) {

    case HTTP_UPDATE_FAILED:

      Serial.printf(
        "❌ Update failed. Error (%d): %s\n",
        httpUpdate.getLastError(),
        httpUpdate.getLastErrorString().c_str());
      otaProgress = 0;

      break;

    case HTTP_UPDATE_NO_UPDATES:

      Serial.println("✅ Обновление не требуется");

      otaProgress = 0;

      break;

    case HTTP_UPDATE_OK:

      otaProgress = 100;

      Serial.println("✅ Обновление успешно");

      break;
  }
}

bool checkVersion() {

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url = String(versionURL) + "?t=" + String(millis());

  https.setTimeout(5000);
  https.setReuse(false);

  if (!https.begin(client, url)) {
    Serial.println("❌ begin failed");
    return false;
  }

  int code = https.GET();

  if (code != HTTP_CODE_OK) {
    Serial.println("❌ HTTP error: " + String(code));
    https.end();
    return false;
  }

  remoteVersion = https.getString();
  remoteVersion.trim();

  https.end();

  updateAvailableFlag = (remoteVersion != String(FW_VERSION));

  Serial.println("FW: " + String(FW_VERSION));
  Serial.println("REMOTE: " + remoteVersion);

  return true;
}

void disableSTA() {

  WiFi.disconnect(true, true);

  WiFi.mode(WIFI_AP);

  staEnabled = false;

  Serial.println("📴 STA Wi-Fi отключен");
}

void enableSTA(String ssid, String pass) {

  WiFi.mode(WIFI_AP_STA);

  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {

    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    staEnabled = true;

    Serial.println("");
    Serial.println("✅ STA подключен");
    Serial.println(WiFi.localIP());

  } else {

    Serial.println("");
    Serial.println("❌ STA connection failed");
  }
}

void factoryReset() {
  EEPROM.begin(EEPROM_SIZE);

  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }

  EEPROM.commit();
  EEPROM.end();

  Serial.println("🧹 Factory reset выполнен. Перезагрузка...");

  delay(500);
  ESP.restart();
}

void saveWelcome() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_WELCOME_ADDR, welcomeCompleted);
  EEPROM.commit();
  EEPROM.end();
}

void loadWelcome() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_WELCOME_ADDR, welcomeCompleted);
  EEPROM.end();
}

String tr(const String& de, const String& ua, const String& en) {
  switch (currentLang) {
    case LANG_UA: return ua;
    case LANG_EN: return en;
    default: return de;
  }
}

String modeName(const String& key) {

  if (key == "MP5") {
    return tr(
      "Sportpistole",  // Deutsch
      "МП5",           // Українська
      "Sport Pistol"   // English
    );
  }

  if (key == "MP8") {
    return tr(
      "Schnellfeuer",  // Deutsch
      "МП8",           // Українська
      "Rapid Fire"     // English
    );
  }

  if (key == "MP10") {
    return tr(
      "Standardpistole",  // Deutsch
      "МП10",             // Українська
      "Standart Pistol"   // English
    );
  }

  return key;
}

void saveLanguage() {
  EEPROM.begin(EEPROM_SIZE);

  uint8_t lang = (uint8_t)currentLang;

  EEPROM.put(EEPROM_LANG_ADDR, setupCompleted);
  EEPROM.put(EEPROM_LANG_ADDR + 1, lang);

  EEPROM.commit();
  EEPROM.end();
}

void loadLanguage() {
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.get(EEPROM_LANG_ADDR, setupCompleted);

  uint8_t lang;
  EEPROM.get(EEPROM_LANG_ADDR + 1, lang);

  EEPROM.end();

  if (lang <= LANG_EN) {
    currentLang = (Language)lang;
  } else {
    currentLang = LANG_DE;
  }
}

// ====== URL helpers ======
String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.length()) {
      char h1 = s[i + 1], h2 = s[i + 2];
      auto hexVal = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return 0;
      };
      char v = (hexVal(h1) << 4) | hexVal(h2);
      out += v;
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String getParam(const String& req, const String& key) {
  int qsStart = req.indexOf("GET ");
  if (qsStart == -1) return "";
  int pathStart = qsStart + 4;
  int spaceAfterPath = req.indexOf(' ', pathStart);
  if (spaceAfterPath == -1) return "";
  String path = req.substring(pathStart, spaceAfterPath);
  int q = path.indexOf('?');
  if (q == -1) return "";
  String query = path.substring(q + 1);
  int start = 0;
  while (start < query.length()) {
    int amp = query.indexOf('&', start);
    if (amp == -1) amp = query.length();
    int eq = query.indexOf('=', start);
    if (eq != -1 && eq < amp) {
      String k = query.substring(start, eq);
      String v = query.substring(eq + 1, amp);
      if (k == key) return urlDecode(v);
    }
    start = amp + 1;
  }
  return "";
}

// ====== Wi-Fi AP ======
void setupWiFi() {
  IPAddress local_IP(192, 168, 2, 5);
  IPAddress gateway(192, 168, 2, 5);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssid, password);
  server.begin();

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.println("✅ WiFi AP запущен");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("PASS: ");
  Serial.println(password);
  Serial.println("IP: 192.168.2.5");
}

// ====== Program control ======
void startProgram(uint16_t redSec, uint16_t greenSec, uint16_t seriesCount) {
  if (redSec == 0 || greenSec == 0 || seriesCount == 0) return;
  redDuration = (unsigned long)redSec * 1000UL;
  greenDuration = (unsigned long)greenSec * 1000UL;
  totalStages = seriesCount;
  currentStage = 0;
  isProgramRunning = true;
  stopRequested = false;
  stageRed = true;
  stageStartTime = millis();
  finishingRed = false;

  // Сохраняем для кнопки + EEPROM
  lastRed = redSec;
  lastGreen = greenSec;
  lastSeries = seriesCount;
  saveLastProgram();

  Serial.printf("▶️ Старт: R=%us G=%us series=%u\n", redSec, greenSec, seriesCount);
}

void stopProgram() {
  stopRequested = true;
  activeProgramId = -1;
  digitalWrite(redLedPin, LOW);
  digitalWrite(greenLedPin, LOW);
  isProgramRunning = false;
  activeProgramId = -1;
  finishingRed = false;
  Serial.println("⏹ Остановлено");
}

void handleProgramLogic() {
  if (!isProgramRunning || stopRequested) return;

  unsigned long now = millis();

  if (currentStage >= totalStages) {
    if (!finishingRed) {
      digitalWrite(redLedPin, HIGH);
      digitalWrite(greenLedPin, LOW);
      finishStartTime = now;
      finishingRed = true;
      Serial.println("🟥 Завершение: красный 7 сек");
    } else if (now - finishStartTime >= 7000UL) {
      digitalWrite(redLedPin, LOW);
      isProgramRunning = false;
      stopRequested = false;
      finishingRed = false;
      Serial.println("✅ Завершено");
    }
    return;
  }

  if (stageRed) {
    digitalWrite(redLedPin, HIGH);
    digitalWrite(greenLedPin, LOW);
    if (now - stageStartTime >= redDuration) {
      stageRed = false;
      stageStartTime = now;
    }
  } else {
    digitalWrite(redLedPin, LOW);
    digitalWrite(greenLedPin, HIGH);
    if (now - stageStartTime >= greenDuration) {
      currentStage++;
      stageRed = true;
      stageStartTime = now;
    }
  }
}

// ====== Button ======
void handleButton() {
  static unsigned long lastChange = 0;
  static bool lastState = true;
  bool state = digitalRead(buttonPin);  // PULLUP: LOW=pressed
  unsigned long now = millis();

  if (state != lastState && (now - lastChange) > 40) {
    lastChange = now;
    lastState = state;
    if (state == LOW) {  // нажата
      if (isProgramRunning) {
        stopProgram();
      } else {
        if (lastRed > 0 && lastGreen > 0 && lastSeries > 0) {
          startProgram(lastRed, lastGreen, lastSeries);
        } else {
          Serial.println("⚠️ Нет сохранённой программы для запуска кнопкой");
        }
      }
    }
  }
}
// ====== UI HTML ======
String htmlHeader() {
  return "<!DOCTYPE html><html lang='ru'><head><meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
         "<style>"
         /* ===== Стиль страницы ===== */
         "body{margin:0;font-family:'Segoe UI',sans-serif;background:#1c1c1c;color:#fff;display:flex;flex-direction:column;align-items:center;}"
         "header{background:#2d2d2d;width:100%;padding:20px;text-align:center;font-size:24px;font-weight:bold;color:#FFD700;}"
         ".grid{display:grid;grid-template-columns:1fr 1fr;gap:15px;padding:20px;width:100%;max-width:480px;}"
         ".btn{padding:16px;text-align:center;border-radius:12px;background:#0057B7;color:#fff;text-decoration:none;font-size:18px;}"
         ".btn:hover{background:#0043a0;} .yellow{background:#FFD700;color:#000;} .yellow:hover{background:#e6c200;}"
         ".footer{margin-top:auto;padding:20px;text-align:center;font-size:18px;color:#FFD700;}"
         "form{display:flex;flex-direction:column;gap:12px;padding:20px;width:100%;max-width:480px;}"
         "input{padding:12px;border-radius:10px;border:none;font-size:16px;} button{padding:14px;background:#FFD700;color:#000;border:none;border-radius:12px;font-size:18px;cursor:pointer;} button:hover{background:#e6c200;} .row{display:flex;gap:10px;} .tag{padding:6px 10px;border-radius:999px;background:#2d2d2d;color:#FFD700;font-size:14px;margin:10px 0;}"

         /* ===== Блок защиты от копирования ===== */
         "* {"
         "-webkit-user-select: none;"
         "-moz-user-select: none;"
         "-ms-user-select: none;"
         "user-select: none;"
         "-webkit-touch-callout: none;"
         "-webkit-tap-highlight-color: transparent;"
         "}"
         "input, textarea, select {"
         "user-select: text;"
         "-webkit-user-select: text;"
         "}"
         "</style>"

         "<script>"
         "// Запрещаем контекстное меню"
         "document.addEventListener('contextmenu', function(e){ e.preventDefault(); });"
         "// Запрещаем копирование и вырезание"
         "document.addEventListener('copy', function(e){ e.preventDefault(); });"
         "document.addEventListener('cut', function(e){ e.preventDefault(); });"
         "</script>"

         "</head><body>";
}

String welcomePage() {
  String html = R"raw(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>TargetiX Device</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --border: #e2e8f0;
      --status-online: #10b981;
      --status-busy: #f59e0b;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    body {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      color: var(--text-main);
      overflow: hidden;
      animation: ambientBreath 8s ease-in-out infinite alternate;
    }

    @keyframes ambientBreath {
      0% { background-position: 0% 0%, 0 0; }
      100% { background-position: 0% 0%, 12px 12px; }
    }

    .card {
      position: relative;
      width: min(340px, 90vw);
      padding: 36px 28px;
      border-radius: 24px;
      background: var(--card-bg);
      border: 1px solid var(--border);
      box-shadow: 0 20px 30px -10px rgba(15, 23, 42, 0.03), 
                  0 8px 12px -6px rgba(15, 23, 42, 0.02);
      text-align: center;
      opacity: 0;
      transform: translateY(20px) scale(0.96);
      animation: cardPopIn 0.8s var(--cubic-smooth) forwards;
      transition: transform 0.4s var(--cubic-smooth), 
                  box-shadow 0.4s var(--cubic-smooth), 
                  opacity 0.4s var(--cubic-smooth);
    }

    @keyframes cardPopIn {
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 6px 14px;
      border-radius: 20px;
      background: var(--card-hover);
      border: 1px solid var(--border);
      font-size: 0.75rem;
      font-weight: 600;
      color: var(--text-sub);
      margin-bottom: 20px;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards;
    }

    .status-dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background-color: var(--status-busy);
      box-shadow: 0 0 8px var(--status-busy);
      animation: pulseGlow 2s ease-in-out infinite;
      transition: background-color 0.3s ease, box-shadow 0.3s ease;
    }

    .status-dot.online {
      background-color: var(--status-online);
      box-shadow: 0 0 8px var(--status-online);
    }

    @keyframes pulseGlow {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.4; }
    }

    .logo {
      font-size: 2.2rem;
      font-weight: 800;
      letter-spacing: -0.03em;
      color: var(--text-main);
      margin-bottom: 4px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards;
    }

    .sub {
      font-size: 0.9rem;
      font-weight: 500;
      color: var(--text-sub);
      margin-bottom: 28px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.4s forwards;
    }

    .lang-switcher {
      display: flex;
      justify-content: center;
      gap: 6px;
      margin-bottom: 24px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.45s forwards;
    }

    .lang-btn {
      background: transparent;
      border: 1px solid var(--border);
      color: var(--text-sub);
      padding: 4px 10px;
      font-size: 0.75rem;
      font-weight: 600;
      border-radius: 8px;
      cursor: pointer;
      transition: all 0.2s var(--cubic-smooth);
    }

    .lang-btn.active, .lang-btn:hover {
      background: var(--card-hover);
      color: var(--text-main);
      border-color: var(--text-sub);
    }

    .btn {
      position: relative;
      width: 100%;
      padding: 16px 0;
      border-radius: 14px;
      font-size: 1rem;
      font-weight: 600;
      color: #ffffff;
      background: var(--primary);
      border: none;
      cursor: not-allowed;
      outline: none;
      opacity: 0.5;
      box-shadow: none;
      transition: all 0.3s var(--cubic-smooth);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      overflow: hidden;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.5s forwards;
    }

    .btn.enabled {
      cursor: pointer;
      opacity: 1;
      box-shadow: 0 8px 20px -4px rgba(37, 99, 235, 0.35);
    }

    .btn.enabled::before {
      content: '';
      position: absolute;
      inset: 0;
      background: var(--primary-hover);
      opacity: 0;
      transition: opacity 0.3s var(--cubic-smooth);
      z-index: 0;
    }

    .btn span, .btn svg {
      position: relative;
      z-index: 1;
      transition: transform 0.3s var(--cubic-smooth);
    }

    .btn.enabled:hover {
      box-shadow: 0 12px 24px -4px rgba(37, 99, 235, 0.45);
      transform: translateY(-2px);
    }

    .btn.enabled:hover::before {
      opacity: 1;
    }

    .btn.enabled:hover svg {
      transform: translateX(4px);
    }

    .btn.enabled:active {
      transform: translateY(1px) scale(0.98);
      box-shadow: 0 4px 10px -2px rgba(37, 99, 235, 0.25);
    }

    .spinner {
      width: 18px;
      height: 18px;
      border: 2px solid rgba(255,255,255,0.3);
      border-radius: 50%;
      border-top-color: #fff;
      animation: spin 0.8s linear infinite;
      display: none;
    }

    @keyframes spin {
      to { transform: rotate(360deg); }
    }

    .footer {
      position: absolute;
      bottom: 24px;
      font-size: 0.8rem;
      font-weight: 500;
      color: var(--text-sub);
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.6s forwards;
    }

    @keyframes fadeUp {
      from {
        opacity: 0;
        transform: translateY(12px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .fade-out {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.95) !important;
      transition: opacity 0.35s var(--cubic-smooth), 
                  transform 0.35s var(--cubic-smooth) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());

    // Полный словарь для всех языков интерфейса
    const dict = {
      de: { ready: "Bereit", busy: "Gerät beschäftigt", start: "Setup starten", loading: "Wird geladen..." },
      ua: { ready: "Готовий до роботи", busy: "Пристрій зайнятий", start: "Почати налаштування", loading: "Завантаження..." },
      en: { ready: "Ready", busy: "Device busy", start: "Start setup", loading: "Loading..." }
    };

    let currentLang = 'ua'; // Язык по умолчанию
    let isDeviceReady = false; // Статус готовности устройства

    function setLanguage(lang) {
      currentLang = lang;
      // Подсветка активной кнопки языка
      document.querySelectorAll('.lang-btn').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.lang === lang);
      });
      // Обновляем текст кнопки запуска
      document.getElementById('btnText').innerText = dict[lang].start;
      // Обновляем статус с учетом текущего языка
      updateUIStatus(isDeviceReady);
    }

    function updateUIStatus(ready) {
      isDeviceReady = ready;
      const dot = document.getElementById('statusDot');
      const text = document.getElementById('statusText');
      const btn = document.getElementById('setupBtn');

      if (ready) {
        dot.classList.add('online');
        text.innerText = dict[currentLang].ready;
        btn.classList.add('enabled');
      } else {
        dot.classList.remove('online');
        text.innerText = dict[currentLang].busy;
        btn.classList.remove('enabled');
      }
    }

    // Опрос устройства о готовности (каждые 2 секунды)
    function checkDeviceStatus() {
      fetch('/api/ready')
        .then(res => res.json())
        .then(data => {
          updateUIStatus(data.ready === true);
        })
        .catch(err => {
          updateUIStatus(false);
        });
    }

    setInterval(checkDeviceStatus, 2000);
    window.onload = checkDeviceStatus;

    function clickSound() {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(520, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(300, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.08, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    }

    function goSetup(e) {
      if (!isDeviceReady) return; // Защита от нажатия, если не готово
      clickSound();
      
      const btn = document.getElementById('setupBtn');
      const btnText = document.getElementById('btnText');
      const spinner = document.getElementById('btnSpinner');
      const svgIcon = document.getElementById('btnSvg');

      btnText.innerText = dict[currentLang].loading;
      spinner.style.display = 'block';
      if (svgIcon) svgIcon.style.display = 'none';
      btn.style.pointerEvents = 'none';

      const card = document.querySelector('.card');
      card.classList.add('fade-out');
      setTimeout(() => {
        location.href = '/STARTSETUP';
      }, 350);
    }
  </script>
</head>
<body>

  <div class="card">
    <!-- Переключатель языка -->
    <div class="lang-switcher">
      <button class="lang-btn" data-lang="de" onclick="setLanguage('de')">DE</button>
      <button class="lang-btn active" data-lang="ua" onclick="setLanguage('ua')">UA</button>
      <button class="lang-btn" data-lang="en" onclick="setLanguage('en')">EN</button>
    </div>

    <div class="status-badge">
      <span class="status-dot" id="statusDot"></span> 
      <span id="statusText">Завантаження...</span>
    </div>
    
    <div class="logo">TargetiX</div>
    <div class="sub">Professional Simulator</div>

    <button class="btn" id="setupBtn" onclick="goSetup(event)">
      <span id="btnText">Почати налаштування</span>
      <div class="spinner" id="btnSpinner"></div>
      <svg id="btnSvg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <path d="M5 12h14M12 5l7 7-7 7"/>
      </svg>
    </button>
  </div>

  <div class="footer">TargetiX PRO &bull; Device Web UI</div>

</body>
</html>
)raw";

  return html;
}

String setupPage() {
  // Заранее определяем локализованный подзаголовок
  String subtitleText = tr("Wählen Sie die Sprache", "Оберіть мову інтерфейсу", "Select interface language");

  // Собираем HTML, разбивая сырой литерал
  String html = R"raw(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>TargetiX Setup</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --border: #e2e8f0;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    body {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      color: var(--text-main);
      padding: 20px;
      overflow: hidden;
      animation: ambientBreath 8s ease-in-out infinite alternate;
    }

    @keyframes ambientBreath {
      0% { background-position: 0% 0%, 0 0; }
      100% { background-position: 0% 0%, 12px 12px; }
    }

    .container {
      width: min(360px, 100%);
      display: flex;
      flex-direction: column;
      align-items: center;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
    }

    .header {
      text-align: center;
      margin-bottom: 24px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards;
    }

    .title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
      margin-bottom: 4px;
    }

    .subtitle {
      font-size: 0.875rem;
      color: var(--text-sub);
      font-weight: 500;
    }

    .card {
      width: 100%;
      background: var(--card-bg);
      padding: 16px 20px;
      margin-bottom: 12px;
      border-radius: 18px;
      border: 1px solid var(--border);
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03), 
                  0 2px 4px -2px rgba(15, 23, 42, 0.02);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: space-between;
      opacity: 0;
      transition: transform 0.3s var(--cubic-smooth), 
                  background-color 0.3s var(--cubic-smooth), 
                  border-color 0.3s var(--cubic-smooth), 
                  box-shadow 0.3s var(--cubic-smooth);
    }

    .card:nth-child(2) { animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards; }
    .card:nth-child(3) { animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards; }
    .card:nth-child(4) { animation: fadeUp 0.6s var(--cubic-smooth) 0.4s forwards; }

    .card:hover {
      background: var(--card-hover);
      border-color: var(--text-sub);
      transform: translateY(-2px);
      box-shadow: 0 10px 20px -4px rgba(15, 23, 42, 0.08);
    }

    .card:active {
      transform: translateY(1px) scale(0.98);
      box-shadow: 0 2px 6px -1px rgba(15, 23, 42, 0.05);
    }

    .lang-info {
      display: flex;
      align-items: center;
      gap: 12px;
    }

    .flag {
      font-size: 1.5rem;
      line-height: 1;
      transition: transform 0.3s var(--cubic-smooth);
    }

    .card:hover .flag {
      transform: scale(1.1);
    }

    .lang-name {
      font-size: 1.05rem;
      font-weight: 600;
      color: var(--text-main);
    }

    .arrow {
      color: var(--text-sub);
      opacity: 0.4;
      font-size: 1.2rem;
      transition: transform 0.3s var(--cubic-smooth), opacity 0.3s var(--cubic-smooth);
    }

    .card:hover .arrow {
      opacity: 1;
      transform: translateX(4px);
    }

    @keyframes fadeUp {
      from {
        opacity: 0;
        transform: translateY(14px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());

    function clickSound() {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        
        o.type = 'sine';
        o.frequency.setValueAtTime(600, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(350, ctx.currentTime + 0.08);
        
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        
        o.connect(g);
        g.connect(ctx.destination);
        
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    }

    function goMain(event, url) {
      clickSound();
      const container = document.querySelector('.container');
      container.classList.add('fadeOut');
      setTimeout(() => {
        location.href = url;
      }, 300);
    }
  </script>
</head>
<body>

  <div class="container">
    <div class="header">
      <div class="title">Language / Язык / Sprache</div>
      <div class="subtitle">)raw"
                + subtitleText + R"raw(</div>
    </div>

    <div class="card" onclick="goMain(event, '/SETLANG?l=en')">
      <div class="lang-info">
        <span class="flag">🇬🇧</span>
        <span class="lang-name">English</span>
      </div>
      <span class="arrow">&rsaquo;</span>
    </div>

    <div class="card" onclick="goMain(event, '/SETLANG?l=ua')">
      <div class="lang-info">
        <span class="flag">🇺🇦</span>
        <span class="lang-name">Українська</span>
      </div>
      <span class="arrow">&rsaquo;</span>
    </div>

    <div class="card" onclick="goMain(event, '/SETLANG?l=de')">
      <div class="lang-info">
        <span class="flag">🇩🇪</span>
        <span class="lang-name">Deutsch</span>
      </div>
      <span class="arrow">&rsaquo;</span>
    </div>
  </div>

</body>
</html>
)raw";

  return html;
}



// Вспомогательная функция для экранирования HTML (защита от спецсимволов)
String htmlspecialchars(String str) {
  str.replace("&", "&amp;");
  str.replace("\"", "&quot;");
  str.replace("'", "&#039;");
  str.replace("<", "&lt;");
  str.replace(">", "&gt;");
  return str;
}

String wifiSetupPage() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  // Обработка запроса на принудительное сканирование (например, /wifisetup?scan=1)
  // Убедитесь, что в обработтере сервера вы передаете этот параметр или вызываете сканирование
  int n = 0;
  if (!isConnected) {
    n = WiFi.scanNetworks();
  }

  String html = "";
  html.reserve(6144);  // Резервируем память для снижения фрагментации

  html += R"raw(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
  <title>TargetiX — Wi-Fi Setup</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --border: #e2e8f0;
      --status-online: #10b981;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    input {
      user-select: text;
    }

    body {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      color: var(--text-main);
      padding: 20px;
      overflow-x: hidden;
    }

    .container {
      width: min(390px, 100%);
      display: flex;
      flex-direction: column;
      align-items: center;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
    }

    .header {
      text-align: center;
      margin-bottom: 20px;
      width: 100%;
      position: relative;
    }

    .title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }

    .subtitle {
      font-size: 0.875rem;
      color: var(--text-sub);
      font-weight: 500;
      margin-top: 4px;
    }

    /* Кнопка обновления в шапке */
    .refresh-btn {
      position: absolute;
      right: 0;
      top: 50%;
      transform: translateY(-50%);
      background: var(--card-bg);
      border: 1px solid var(--border);
      width: 38px;
      height: 38px;
      border-radius: 12px;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      color: var(--text-sub);
      transition: all 0.3s var(--cubic-smooth);
      box-shadow: 0 2px 8px rgba(0,0,0,0.02);
    }

    .refresh-btn:hover {
      background: var(--card-hover);
      color: var(--primary);
      border-color: var(--primary);
    }

    .refresh-btn svg.spin {
      animation: spin 0.8s linear infinite;
    }

    .card {
      width: 100%;
      background: var(--card-bg);
      padding: 24px;
      border-radius: 24px;
      border: 1px solid var(--border);
      box-shadow: 0 10px 25px -5px rgba(15, 23, 42, 0.03), 
                  0 8px 10px -6px rgba(15, 23, 42, 0.02);
    }

    input {
      width: 100%;
      padding: 14px 16px;
      border-radius: 14px;
      font-size: 0.95rem;
      font-weight: 500;
      background: var(--bg-color);
      border: 1px solid var(--border);
      color: var(--text-main);
      outline: none;
      transition: all 0.3s var(--cubic-smooth);
    }

    input:focus {
      background: var(--card-bg);
      border-color: var(--primary);
      box-shadow: 0 0 0 4px rgba(37, 99, 235, 0.12);
    }

    /* Кастомный селект */
    .custom-select-wrapper {
      position: relative;
      user-select: none;
      width: 100%;
      margin-bottom: 12px;
    }

    .custom-select {
      position: relative;
      display: flex;
      flex-direction: column;
    }

    .custom-select__trigger {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 14px 16px;
      font-size: 0.95rem;
      font-weight: 500;
      background: var(--bg-color);
      border: 1px solid var(--border);
      color: var(--text-main);
      border-radius: 14px;
      cursor: pointer;
      transition: all 0.3s var(--cubic-smooth);
    }

    .custom-select__trigger svg {
      transition: transform 0.3s var(--cubic-smooth);
      stroke: var(--text-sub);
    }

    .custom-select.open .custom-select__trigger {
      background: var(--card-bg);
      border-color: var(--primary);
      box-shadow: 0 0 0 4px rgba(37, 99, 235, 0.12);
    }

    .custom-select.open .custom-select__trigger svg {
      transform: rotate(180deg);
      stroke: var(--primary);
    }

    .custom-options {
      position: absolute;
      top: calc(100% + 8px);
      left: 0;
      right: 0;
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 16px;
      box-shadow: 0 15px 30px -5px rgba(15, 23, 42, 0.1);
      z-index: 99;
      opacity: 0;
      visibility: hidden;
      pointer-events: none;
      transform: translateY(-8px);
      transition: all 0.3s var(--cubic-smooth);
      max-height: 220px;
      overflow-y: auto;
    }

    .custom-select.open .custom-options {
      opacity: 1;
      visibility: visible;
      pointer-events: auto;
      transform: translateY(0);
    }

    .custom-option {
      padding: 12px 16px;
      font-size: 0.9rem;
      font-weight: 500;
      color: var(--text-main);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: space-between;
      transition: background 0.2s;
      border-bottom: 1px solid var(--border);
    }

    .custom-option:last-child {
      border-bottom: none;
    }

    .custom-option:hover {
      background: var(--card-hover);
      color: var(--primary);
    }

    .custom-option.selected {
      background: var(--card-hover);
      font-weight: 600;
      color: var(--primary);
    }

    .net-meta {
      display: flex;
      align-items: center;
      gap: 6px;
      font-size: 0.8rem;
      color: var(--text-sub);
    }

    /* Анимация скрытых элементов */
    .collapse-box {
      max-height: 0;
      opacity: 0;
      overflow: hidden;
      margin-top: 0;
      transition: max-height 0.35s var(--cubic-smooth), 
                  opacity 0.3s var(--cubic-smooth), 
                  margin-top 0.3s var(--cubic-smooth);
    }

    .collapse-box.show {
      max-height: 80px;
      opacity: 1;
      margin-top: 12px;
    }

    .btn {
      position: relative;
      width: 100%;
      padding: 15px 0;
      margin-top: 16px;
      border-radius: 14px;
      font-size: 0.95rem;
      font-weight: 600;
      color: #ffffff;
      background: var(--primary);
      border: none;
      cursor: pointer;
      box-shadow: 0 8px 18px -4px rgba(37, 99, 235, 0.35);
      transition: transform 0.3s var(--cubic-smooth), box-shadow 0.3s var(--cubic-smooth);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }

    .btn:hover {
      box-shadow: 0 12px 22px -4px rgba(37, 99, 235, 0.45);
      transform: translateY(-2px);
    }

    .btn:active { transform: translateY(1px) scale(0.98); }

    .btn-back {
      width: 100%;
      padding: 14px 0;
      margin-top: 12px;
      border-radius: 14px;
      font-size: 0.9rem;
      font-weight: 600;
      color: var(--text-sub);
      background: var(--card-hover);
      border: 1px solid var(--border);
      cursor: pointer;
      text-align: center;
      transition: all 0.3s var(--cubic-smooth);
    }

    .btn-back:hover {
      background: var(--border);
      color: var(--text-main);
      transform: translateY(-1px);
    }

    /* Статус подключения */
    .connected-box {
      text-align: center;
      padding: 10px 0;
    }

    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 6px 16px;
      border-radius: 20px;
      background: #dcfce7;
      color: #15803d;
      font-size: 0.85rem;
      font-weight: 700;
      margin-bottom: 12px;
    }

    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background-color: var(--status-online);
      box-shadow: 0 0 8px var(--status-online);
      animation: pulseGlow 2s ease-in-out infinite;
    }

    .ssid-name {
      font-size: 1.25rem;
      font-weight: 700;
      color: var(--text-main);
      word-break: break-all;
    }

    /* Модальное окно */
    .modal-overlay {
      position: fixed;
      inset: 0;
      background: rgba(15, 23, 42, 0.6);
      backdrop-filter: blur(8px);
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 20px;
      z-index: 1000;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.3s var(--cubic-smooth);
    }

    .modal-overlay.active {
      opacity: 1;
      pointer-events: auto;
    }

    .modal-card {
      width: min(320px, 100%);
      background: var(--card-bg);
      padding: 28px 24px;
      border-radius: 24px;
      border: 1px solid var(--border);
      box-shadow: 0 20px 25px -5px rgba(15, 23, 42, 0.1);
      text-align: center;
      transform: scale(0.94) translateY(10px);
      transition: transform 0.35s var(--cubic-smooth);
    }

    .modal-overlay.active .modal-card {
      transform: scale(1) translateY(0);
    }

    .spinner {
      width: 48px;
      height: 48px;
      border: 4px solid var(--border);
      border-top-color: var(--primary);
      border-radius: 50%;
      margin: 0 auto 18px;
      animation: spin 0.8s linear infinite;
    }

    @keyframes spin { to { transform: rotate(360deg); } }
    @keyframes pulseGlow { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }

    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());

    const clickSound = () => {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(550, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}
      try { if (navigator.vibrate) navigator.vibrate(40); } catch(e) {}
    };

    // Логика кастомного селектора
    document.addEventListener('DOMContentLoaded', () => {
      const selectCustom = document.querySelector('.custom-select');
      const trigger = document.querySelector('.custom-select__trigger');
      const options = document.querySelectorAll('.custom-option');
      const hiddenInput = document.getElementById('ssid_input');
      const passBox = document.getElementById('passBox');
      const hiddenSsidBox = document.getElementById('hiddenSsidBox');
      const customSsidInput = document.getElementById('custom_ssid_input');

      trigger.addEventListener('click', () => {
        clickSound();
        selectCustom.classList.toggle('open');
      });

      options.forEach(option => {
        option.addEventListener('click', () => {
          clickSound();
          options.forEach(opt => opt.classList.remove('selected'));
          option.classList.add('selected');
          
          const value = option.dataset.value;
          const label = option.dataset.label;
          const isOpen = option.dataset.open === '1';

          trigger.querySelector('span').textContent = label;
          hiddenInput.value = value;
          selectCustom.classList.remove('open');

          if (value === '__hidden__') {
            hiddenSsidBox.classList.add('show');
            passBox.classList.add('show');
            customSsidInput.required = true;
          } else {
            hiddenSsidBox.classList.remove('show');
            customSsidInput.required = false;
            customSsidInput.value = '';
            
            if (isOpen) {
              passBox.classList.remove('show');
            } else {
              passBox.classList.add('show');
            }
          }
        });
      });

      // Закрытие селекта при клике вне его области
      window.addEventListener('click', (e) => {
        if (!selectCustom.contains(e.target)) {
          selectCustom.classList.remove('open');
        }
      });
    });

    const refreshNetworks = (btn) => {
      clickSound();
      btn.querySelector('svg').classList.add('spin');
      setTimeout(() => {
        location.href = '/WIFISETUP?scan=1';
      }, 300);
    };

    const goUrl = (url) => {
      clickSound();
      document.querySelector('.container').classList.add('fadeOut');
      setTimeout(() => { location.href = url; }, 300);
    };

    const submitForm = (e) => {
      clickSound();
      document.getElementById('connectingModal').classList.add('active');
    };
  </script>
</head>
<body>

  <div class="container">
    <div class="header">
      <div class="title">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <path d="M5 12.55a11 11 0 0 1 14 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/>
        </svg>
        <span>Wi-Fi Network</span>
      </div>
      <div class="subtitle">)raw"
          + tr("Verbindung zum WLAN-Netzwerk", "Підключення до бездротової мережі", "Connecting to wireless network") + R"raw(</div>
      
      <!-- Кнопка обновления сетей -->
      <div class="refresh-btn" onclick="refreshNetworks(this)" title="Networks aktualisieren">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.73-5.73"/>
        </svg>
      </div>
    </div>
)raw";

  // ===== ПОДКЛЮЧЕНО =====
  if (isConnected) {
    html += R"raw(
    <div class="card">
      <div class="connected-box">
        <div class="status-badge">
          <span class="status-dot"></span> )raw"
            + tr("Verbunden", "Підключено", "Connected") + R"raw(
        </div>
        <div class="ssid-name">)raw";
    html += htmlspecialchars(WiFi.SSID());
    html += R"raw(</div>
      </div>
    </div>
)raw";
  }
  // ===== НЕ ПОДКЛЮЧЕНО =====
  else {
    html += R"raw(
    <div class="card">
      <form action="/CONNECTWIFI" method="get" onsubmit="submitForm(event)">
        
        <!-- Кастомный выпадающий список -->
        <div class="custom-select-wrapper">
          <div class="custom-select">
            <div class="custom-select__trigger">
              <span>)raw"
            + tr("WLAN-Netzwerk auswählen", "Виберіть Wi-Fi мережу", "Select Wi-Fi network") + R"raw(</span>
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M6 9l6 6 6-6"/></svg>
            </div>
            <div class="custom-options">
              <!-- Опция для ручного (скрытого) ввода -->
              <div class="custom-option" data-value="__hidden__" data-label="➕ )raw"
            + tr("Andere (versteckte) Netzwerk...", "Інша (прихована) мережа...", "Other (hidden) network...") + R"raw(" data-open="0">
                <span>➕ )raw"
            + tr("Andere (versteckte) Netzwerk...", "Інша (прихована) мережа...", "Other (hidden) network...") + R"raw(</span>
              </div>
)raw";

    for (int i = 0; i < n; i++) {
      bool openNet = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      String rawSSID = WiFi.SSID(i);
      String safeSSID = htmlspecialchars(rawSSID);

      html += "<div class='custom-option' data-value='" + safeSSID + "' data-label='" + safeSSID + "' data-open='" + String(openNet ? "1" : "0") + "'>";
      html += "<span>" + safeSSID + "</span>";
      html += "<div class='net-meta'><span>" + String(WiFi.RSSI(i)) + " dBm</span>";
      if (openNet) html += " 🔓";
      html += "</div></div>";
    }

    html += R"raw(
            </div>
          </div>
          <input type="hidden" name="ssid" id="ssid_input" required>
        </div>

        <!-- Поле ручного ввода скрытой сети -->
        <div id="hiddenSsidBox" class="collapse-box">
          <input type="text" name="custom_ssid" id="custom_ssid_input" placeholder=")raw"
            + tr("Netzwerkname (SSID)", "Назва мережі (SSID)", "Network name (SSID)") + R"raw(">
        </div>

        <!-- Поле ввода пароля -->
        <div id="passBox" class="collapse-box">
          <input type="password" name="pass" placeholder=")raw"
            + tr("WLAN-Passwort", "Пароль від Wi-Fi", "Wi-Fi password") + R"raw(">
        </div>

        <button class="btn" type="submit">
          <span>)raw"
            + tr("Verbinden", "Підключитися", "Connect") + R"raw(</span>
        </button>
      </form>
    </div>
)raw";
  }

  // Кнопка назад и модальное окно
  html += R"raw(
    <div class="btn-back" onclick="goUrl('/SETTINGS')">
      &#8592; )raw"
          + tr("Zurück zu den Einstellungen", "Назад у налаштування", "Back to settings") + R"raw(
    </div>
  </div>

  <div id="connectingModal" class="modal-overlay">
    <div class="modal-card">
      <div class="spinner"></div>
      <div style="font-size: 1.1rem; font-weight: 700; margin-bottom: 6px;">)raw"
          + tr("Verbindung wird hergestellt...", "Підключення до мережі...", "Connecting to network...") + R"raw(</div>
      <div style="font-size: 0.85rem; color: var(--text-sub); line-height: 1.4;">)raw"
          + tr("Bitte warten, das Gerät verbindet sich mit dem WLAN.", "Будь ласка, зачекайте, пристрій підключається до Wi-Fi.", "Please wait, the device is connecting to Wi-Fi.") + R"raw(</div>
    </div>
  </div>

</body>
</html>
)raw";

  return html;
}

String settingsPage() {
  String html;
  html.reserve(10500);

  html = R"raw(<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>TargetiX — Einstellungen</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --border: #e2e8f0;
      --danger: #ef4444;
      --danger-hover: #dc2626;
      --danger-bg: #fef2f2;
      --warning: #f59e0b;
      --warning-hover: #d97706;
      --warning-bg: #fffbeb;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    input, textarea, select {
      user-select: text;
      font-family: inherit;
    }

    body {
      min-height: 100vh;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      color: var(--text-main);
      padding-bottom: 40px;
      overflow-x: hidden;
    }

    body.modal-open {
      overflow: hidden;
    }

    .container {
      max-width: 440px;
      margin: 0 auto;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
      will-change: transform, opacity;
    }

    header {
      padding: 24px 20px 14px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards;
    }

    .back-btn {
      width: 44px;
      height: 44px;
      border-radius: 14px;
      background: var(--card-bg);
      border: 1px solid var(--border);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--text-main);
      text-decoration: none;
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03);
      transition: all 0.3s var(--cubic-smooth);
    }

    .back-btn:hover {
      background: var(--card-hover);
      border-color: var(--text-sub);
      transform: translateY(-1px);
    }

    .back-btn:active {
      transform: scale(0.92);
    }

    .header-title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .grid {
      padding: 12px 16px;
      display: flex;
      flex-direction: column;
      gap: 14px;
      width: 100%;
    }

    .card {
      background: var(--card-bg);
      padding: 20px 22px;
      border-radius: 22px;
      border: 1px solid var(--border);
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03), 
                  0 2px 4px -2px rgba(15, 23, 42, 0.02);
      display: flex;
      align-items: center;
      justify-content: space-between;
      cursor: pointer;
      position: relative;
      opacity: 0;
      transform: translateZ(0);
      transition: transform 0.3s var(--cubic-smooth), 
                  box-shadow 0.3s var(--cubic-smooth), 
                  border-color 0.3s var(--cubic-smooth),
                  background-color 0.3s var(--cubic-smooth);
    }

    .card:nth-child(1) { animation: fadeUp 0.6s var(--cubic-smooth) 0.15s forwards; }
    .card:nth-child(2) { animation: fadeUp 0.6s var(--cubic-smooth) 0.20s forwards; }
    .card:nth-child(3) { animation: fadeUp 0.6s var(--cubic-smooth) 0.25s forwards; }
    .card:nth-child(4) { animation: fadeUp 0.6s var(--cubic-smooth) 0.30s forwards; }
    .card:nth-child(5) { animation: fadeUp 0.6s var(--cubic-smooth) 0.35s forwards; }
    .card:nth-child(6) { animation: fadeUp 0.6s var(--cubic-smooth) 0.40s forwards; }

    .card:hover {
      transform: translateY(-2px);
      box-shadow: 0 12px 22px -4px rgba(15, 23, 42, 0.08);
      border-color: var(--text-sub);
    }

    .card:active {
      transform: translateY(1px) scale(0.98);
      box-shadow: 0 2px 6px -1px rgba(15, 23, 42, 0.04);
    }

    .card-content {
      display: flex;
      flex-direction: column;
      gap: 4px;
    }

    .title {
      font-size: 1.2rem;
      font-weight: 700;
      color: var(--text-main);
      display: flex;
      align-items: center;
      gap: 10px;
    }

    .sub {
      color: var(--text-sub);
      font-size: 0.9rem;
      font-weight: 500;
    }

    .action-icon {
      width: 38px;
      height: 38px;
      border-radius: 12px;
      background: var(--bg-color);
      border: 1px solid var(--border);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--text-sub);
      transition: all 0.3s var(--cubic-smooth);
      flex-shrink: 0;
    }

    .card:hover .action-icon {
      background: var(--primary);
      border-color: var(--primary);
      color: #ffffff;
      transform: translateX(4px);
    }

    .card.warning-card { border-color: rgba(245, 158, 11, 0.3); }
    .card.warning-card .title { color: var(--warning); }
    .card.warning-card .action-icon { background: var(--warning-bg); border-color: rgba(245, 158, 11, 0.3); color: var(--warning); }
    .card.warning-card:hover { border-color: var(--warning); }
    .card.warning-card:hover .action-icon { background: var(--warning); border-color: var(--warning); color: #ffffff; transform: translateX(4px); }

    .card.danger-card { border-color: rgba(239, 68, 68, 0.3); }
    .card.danger-card .title { color: var(--danger); }
    .card.danger-card .action-icon { background: var(--danger-bg); border-color: rgba(239, 68, 68, 0.3); color: var(--danger); }
    .card.danger-card:hover { border-color: var(--danger); }
    .card.danger-card:hover .action-icon { background: var(--danger); border-color: var(--danger); color: #ffffff; transform: translateX(4px); }

    .overlay {
      position: fixed;
      inset: 0;
      background: rgba(15, 23, 42, 0.6);
      display: flex;
      align-items: center;
      justify-content: center;
      z-index: 100;
      padding: 20px;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.25s var(--cubic-smooth);
    }

    .overlay.active {
      opacity: 1;
      pointer-events: auto;
    }

    .modal {
      background: var(--card-bg);
      border-radius: 24px;
      padding: 28px;
      width: 100%;
      max-width: 360px;
      text-align: center;
      box-shadow: 0 20px 30px -10px rgba(0, 0, 0, 0.3);
      border: 1px solid var(--border);
      transform: scale(0.92) translateY(10px);
      transition: transform 0.25s var(--cubic-smooth);
    }

    .overlay.active .modal {
      transform: scale(1) translateY(0);
    }

    .modal h2 {
      font-size: 1.3rem;
      font-weight: 800;
      color: var(--text-main);
      margin-bottom: 12px;
    }

    .modal.danger-modal h2 { color: var(--danger); }
    .modal.warning-modal h2 { color: var(--warning); }

    .modal p {
      font-size: 0.9rem;
      color: var(--text-sub);
      line-height: 1.6;
      margin-bottom: 20px;
      text-align: left;
    }

    .spinner-box {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 16px;
      padding: 10px 0 20px;
    }

    .spinner {
      width: 48px;
      height: 48px;
      border: 4px solid var(--border);
      border-top-color: var(--primary);
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }

    .spinner.warning { border-top-color: var(--warning); }
    .spinner.danger { border-top-color: var(--danger); }

    @keyframes spin {
      to { transform: rotate(360deg); }
    }

    .lang-select-box {
      display: flex;
      flex-direction: column;
      gap: 10px;
      margin-bottom: 20px;
    }

    .lang-option {
      padding: 14px 16px;
      border-radius: 14px;
      border: 1px solid var(--border);
      background: var(--bg-color);
      color: var(--text-main);
      font-weight: 600;
      font-size: 0.95rem;
      display: flex;
      align-items: center;
      justify-content: space-between;
      cursor: pointer;
      transition: all 0.25s var(--cubic-smooth);
    }

    .lang-option:hover {
      border-color: var(--primary);
      background: var(--card-hover);
    }

    .btn {
      width: 100%;
      padding: 15px;
      border-radius: 14px;
      font-size: 0.95rem;
      font-weight: 700;
      cursor: pointer;
      border: none;
      margin-top: 10px;
      transition: all 0.3s var(--cubic-smooth);
    }

    .btn.primary { background: var(--primary); color: white; box-shadow: 0 6px 16px -4px rgba(37, 99, 235, 0.4); }
    .btn.primary:hover { background: var(--primary-hover); transform: translateY(-1px); }

    .btn.warning { background: var(--warning); color: white; box-shadow: 0 6px 16px -4px rgba(245, 158, 11, 0.4); }
    .btn.warning:hover { background: var(--warning-hover); transform: translateY(-1px); }

    .btn.danger { background: var(--danger); color: white; box-shadow: 0 6px 16px -4px rgba(239, 68, 68, 0.4); }
    .btn.danger:hover { background: var(--danger-hover); transform: translateY(-1px); }

    .btn.cancel { background: var(--card-hover); color: var(--text-main); }
    .btn.cancel:hover { opacity: 0.85; }

    .btn:active { transform: scale(0.97); }

    @keyframes fadeUp {
      from { opacity: 0; transform: translateY(14px); }
      to { opacity: 1; transform: translateY(0); }
    }

    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());

    function clickSound() {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(550, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    }

    function goUrl(url) {
      clickSound();
      const container = document.querySelector('.container');
      container.classList.add('fadeOut');
      setTimeout(() => { location.href = url; }, 300);
    }

    function openModal(id) {
      clickSound();
      document.body.classList.add('modal-open');
      document.getElementById(id).classList.add('active');
    }

    function closeModal(id) {
      clickSound();
      document.getElementById(id).classList.remove('active');
      document.body.classList.remove('modal-open');
    }

    function setLanguage(lang) {
      clickSound();
      closeModal('langOverlay');
      openModal('loadingLangOverlay');

      fetch('/SET_LANG?lang=' + lang, { method: 'GET' })
        .then(response => {
          setTimeout(() => {
            location.reload();
          }, 300);
        })
        .catch(err => {
          setTimeout(() => {
            location.reload();
          }, 300);
        });
    }

    function confirmReboot() {
      clickSound();
      closeModal('rebootOverlay');
      openModal('loadingRebootOverlay');

      fetch('/REBOOT').catch(err => {});

      setTimeout(() => {
        location.href = '/SETTINGS';
      }, 4500);
    }

    function confirmReset() {
      clickSound();
      closeModal('resetOverlay');
      openModal('loadingResetOverlay');
      fetch('/RESET').catch(() => {});
      setTimeout(() => { location.href = '/'; }, 5000);
    }
  </script>
</head>
<body>

  <div class="container">
    <header>
      <a href="javascript:void(0)" onclick="goUrl('/')" class="back-btn">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M19 12H5"/><path d="M12 19l-7-7 7-7"/></svg>
      </a>
      <div class="header-title">
        <span>⚙️ )raw";

  html += tr("Einstellungen", "Налаштування", "Settings");

  html.concat(R"raw(</span>
      </div>
      <div style="width: 44px;"></div>
    </header>

    <div class="grid">
      <!-- Wi-Fi -->
      <div class="card" onclick="goUrl('/WIFISETUP')">
        <div class="card-content">
          <div class="title">📶 Wi-Fi</div>
          <div class="sub">)raw");

  html += tr("Netzwerke & Verbindung", "Мережі та підключення", "Networks & Connection");

  html.concat(R"raw(</div>
        </div>
        <div class="action-icon">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18l6-6-6-6"/></svg>
        </div>
      </div>

      <!-- Язык интерфейса (Временно скрыт) -->
      <!--
      <div class="card" onclick="openModal('langOverlay')">
        <div class="card-content">
          <div class="title">🌐 )raw");

  html += tr("Sprache", "Мова", "Language");

  html.concat(R"raw(</div>
          <div class="sub">Deutsch / Українська / English</div>
        </div>
        <div class="action-icon">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18l6-6-6-6"/></svg>
        </div>
      </div>
      -->

      <!-- OTA Update -->
      <div class="card" onclick="goUrl('/UPDATE')">
        <div class="card-content">
          <div class="title">⬆️ )raw");

  html += tr("Software-Update", "Оновлення ПЗ", "Software Update");

  html.concat(R"raw(</div>
          <div class="sub">)raw");

  html += tr("OTA-Firmware", "Прошивка по повітрю (OTA)", "OTA Firmware");

  html.concat(R"raw(</div>
        </div>
        <div class="action-icon">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18l6-6-6-6"/></svg>
        </div>
      </div>

      <!-- About -->
      <div class="card" onclick="goUrl('/ABOUT')">
        <div class="card-content">
          <div class="title">ℹ️ )raw");

  html += tr("Über das System", "Про систему", "About System");

  html.concat(R"raw(</div>
          <div class="sub">)raw");

  html += tr("Information & Version", "Інформація та версія", "Information & Version");

  html.concat(R"raw(</div>
        </div>
        <div class="action-icon">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18l6-6-6-6"/></svg>
        </div>
      </div>

      <!-- Перезагрузка -->
      <div class="card warning-card" onclick="openModal('rebootOverlay')">
        <div class="card-content">
          <div class="title">🔄 )raw");

  html += tr("Neu starten", "Перезавантажити", "Reboot Device");

  html.concat(R"raw(</div>
          <div class="sub">)raw");

  html += tr("Gerät neustarten", "Перезапуск пристрою", "Restart hardware");

  html.concat(R"raw(</div>
        </div>
        <div class="action-icon">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M21 2v6h-6"/><path d="M3 12a9 9 0 0 1 15-6.7L21 8"/><path d="M3 22v-6h6"/><path d="M21 12a9 9 0 0 1-15 6.7L3 16"/></svg>
        </div>
      </div>

      <!-- Factory Reset -->
      <div class="card danger-card" onclick="openModal('resetOverlay')">
        <div class="card-content">
          <div class="title">🗑️ )raw");

  html += tr("Zurücksetzen", "Скидання налаштувань", "Factory Reset");

  html.concat(R"raw(</div>
          <div class="sub">)raw");

  html += tr("Alle Daten löschen", "Очищення всіх даних", "Clear all data");

  html.concat(R"raw(</div>
        </div>
        <div class="action-icon">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
        </div>
      </div>
    </div>
  </div>

  <!-- Окно выбора языка (Временно скрыто) -->
  <!--
  <div id="langOverlay" class="overlay">
    <div class="modal">
      <h2>🌐 )raw");

  html += tr("Sprache auswählen", "Виберіть мову", "Select Language");

  html.concat(R"raw(</h2>
      <div class="lang-select-box">
        <div class="lang-option" onclick="setLanguage('de')">
          <span>Deutsch</span><span>🇩🇪</span>
        </div>
        <div class="lang-option" onclick="setLanguage('ua')">
          <span>Українська</span><span>🇺🇦</span>
        </div>
        <div class="lang-option" onclick="setLanguage('en')">
          <span>English</span><span>🇬🇧</span>
        </div>
      </div>
      <button class="btn cancel" onclick="closeModal('langOverlay')">)raw");

  html += tr("Abbrechen", "Скасувати", "Cancel");

  html.concat(R"raw(</button>
    </div>
  </div>
  -->

  <!-- Окно ожидания смены языка (Временно скрыто) -->
  <!--
  <div id="loadingLangOverlay" class="overlay">
    <div class="modal">
      <h2>🌐 )raw");

  html += tr("Wird angewendet...", "Застосування...", "Applying...");

  html.concat(R"raw(</h2>
      <div class="spinner-box">
        <div class="spinner"></div>
        <p style="text-align:center; margin-bottom:0;">)raw");

  html += tr("Die Sprache wird geändert, bitte warten...", "Мову змінюється, будь ласка, зачекайте...", "Changing language, please wait...");

  html.concat(R"raw(</p>
      </div>
    </div>
  </div>
  -->

  <!-- Окно подтверждения перезагрузки -->
  <div id="rebootOverlay" class="overlay">
    <div class="modal warning-modal">
      <h2>🔄 )raw");

  html += tr("Neustart", "Перезавантаження", "Reboot");

  html.concat(R"raw(</h2>
      <p>)raw");

  html += tr("Das Gerät wird neu gestartet.<br><br>Die Verbindung wird für einen kurzen Moment unterbrochen.",
             "Пристрій буде перезавантажено.<br><br>З'єднання буде перервано на короткий час.",
             "The device will restart.<br><br>Connection will be briefly interrupted.");

  html.concat(R"raw(</p>
      <button class="btn warning" onclick="confirmReboot()">)raw");

  html += tr("JETZT NEU STARTEN", "ПЕРЕЗАВАНТАЖИТИ", "REBOOT NOW");

  html.concat(R"raw(</button>
      <button class="btn cancel" onclick="closeModal('rebootOverlay')">)raw");

  html += tr("Abbrechen", "Скасувати", "Cancel");

  html.concat(R"raw(</button>
    </div>
  </div>

  <!-- Окно ожидания перезагрузки -->
  <div id="loadingRebootOverlay" class="overlay">
    <div class="modal warning-modal">
      <h2>🔄 )raw");

  html += tr("Neustart läuft", "Перезавантаження", "Rebooting");

  html.concat(R"raw(</h2>
      <div class="spinner-box">
        <div class="spinner warning"></div>
        <p style="text-align:center; margin-bottom:0;">)raw");

  html += tr("Das Gerät startet neu.<br>Bitte warten Sie, die Verbindung wird gleich wiederhergestellt...",
             "Пристрій перезавантажується.<br>Будь ласка, зачекайте, з'єднання скоро відновиться...",
             "Device is restarting.<br>Please wait, connection will be restored shortly...");

  html.concat(R"raw(</p>
      </div>
    </div>
  </div>

  <!-- Окно подтверждения сброса -->
  <div id="resetOverlay" class="overlay">
    <div class="modal danger-modal">
      <h2>⚠️ )raw");

  html += tr("Zurücksetzen", "Скидання налаштувань", "Factory Reset");

  html.concat(R"raw(</h2>
      <p>)raw");

  html += tr("Alle gespeicherten Daten werden gelöscht:<br><br>• Wi-Fi-Netzwerkparameter<br>• Benutzerprogramme<br>• Geräteeinstellungen<br><br>Sind Sie sicher?",
             "Усі збережені дані будуть видалені:<br><br>• Параметри Wi-Fi мережі<br>• Користувацькі програми<br>• Налаштування пристрою<br><br>Ви впевнені?",
             "All saved data will be deleted:<br><br>• Wi-Fi network parameters<br>• Custom programs<br>• Device settings<br><br>Are you sure?");

  html.concat(R"raw(</p>
      <button class="btn danger" onclick="confirmReset()">)raw");

  html += tr("ALLES LÖSCHEN", "ВИДАЛИТИ ВСЕ", "DELETE ALL");

  html.concat(R"raw(</button>
      <button class="btn cancel" onclick="closeModal('resetOverlay')">)raw");

  html += tr("Abbrechen", "Скасувати", "Cancel");

  html.concat(R"raw(</button>
    </div>
  </div>

  <!-- Окно ожидания сброса -->
  <div id="loadingResetOverlay" class="overlay">
    <div class="modal danger-modal">
      <h2>⚠️ )raw");

  html += tr("Wird zurückgesetzt", "Скидання", "Resetting");

  html.concat(R"raw(</h2>
      <div class="spinner-box">
        <div class="spinner danger"></div>
        <p style="text-align:center; margin-bottom:0;">)raw");

  html += tr("Werkseinstellungen werden wiederhergestellt.<br>Das Gerät wird neu gestartet...",
             "Відновлюються заводські налаштування.<br>Пристрій перезавантажується...",
             "Restoring factory settings.<br>Device is restarting...");

  html.concat(R"raw(</p>
      </div>
    </div>
  </div>

</body>
</html>
)raw");

  return html;
}

String mainPage() {
  String html;
  html.reserve(13500);

  html += F("<!DOCTYPE html><html lang='ru'><head>");
  html += F("<meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>");
  html += F("<title>TargetiX - Главная</title>");
  html += F("<style>");
  html += F(":root { --bg-color: #f8fafc; --card-bg: #ffffff; --card-hover: #f1f5f9; --text-main: #0f172a; --text-sub: #64748b; --primary: #2563eb; --primary-hover: #1d4ed8; --border: #e2e8f0; --danger: #ef4444; --danger-hover: #dc2626; --badge-full-bg: #dcfce7; --badge-full-text: #15803d; --badge-lock-bg: #e2e8f0; --badge-lock-text: #64748b; --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1); }");
  html += F("@media (prefers-color-scheme: dark) { :root { --bg-color: #0f172a; --card-bg: #1e293b; --card-hover: #334155; --text-main: #f8fafc; --text-sub: #94a3b8; --border: #334155; } }");
  html += F("* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; user-select: none; }");
  html += F("input, textarea { user-select: text; }");
  html += F("body { min-height: 100vh; background-color: var(--bg-color); background-image: radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%), radial-gradient(var(--border) 1px, transparent 1px); background-size: 100% 100%, 24px 24px; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; color: var(--text-main); padding-bottom: 40px; overflow-x: hidden; animation: ambientBreath 8s ease-in-out infinite alternate; }");
  html += F("@keyframes ambientBreath { 0% { background-position: 0% 0%, 0 0; } 100% { background-position: 0% 0%, 12px 12px; } }");
  html += F(".container { max-width: 440px; margin: 0 auto; transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth); }");
  html += F("header { padding: 24px 20px 14px; display: flex; align-items: center; justify-content: space-between; opacity: 0; animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards; }");
  html += F(".header-title { font-size: 1.8rem; font-weight: 900; letter-spacing: -0.03em; color: var(--text-main); }");
  html += F(".grid { padding: 12px 16px; display: flex; flex-direction: column; gap: 14px; width: 100%; }");
  html += F(".card { background: var(--card-bg); padding: 20px 22px; border-radius: 22px; border: 1px solid var(--border); box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03), 0 2px 4px -2px rgba(15, 23, 42, 0.02); display: flex; align-items: center; justify-content: space-between; cursor: pointer; position: relative; opacity: 0; transition: transform 0.3s var(--cubic-smooth), box-shadow 0.3s var(--cubic-smooth), border-color 0.3s var(--cubic-smooth), opacity 0.3s var(--cubic-smooth); }");
  html += F(".card:nth-child(1) { animation: fadeUp 0.6s var(--cubic-smooth) 0.15s forwards; }");
  html += F(".card:nth-child(2) { animation: fadeUp 0.6s var(--cubic-smooth) 0.25s forwards; }");
  html += F(".card:nth-child(3) { animation: fadeUp 0.6s var(--cubic-smooth) 0.35s forwards; }");
  html += F(".card:nth-child(4) { animation: fadeUp 0.6s var(--cubic-smooth) 0.45s forwards; }");
  html += F(".card:nth-child(n+5) { animation: fadeUp 0.6s var(--cubic-smooth) 0.55s forwards; }");
  html += F(".card:hover:not(.locked-card):not(.navigating) { transform: translateY(-2px); box-shadow: 0 12px 22px -4px rgba(15, 23, 42, 0.08); border-color: var(--text-sub); }");
  html += F(".card:active:not(.locked-card) { transform: translateY(1px) scale(0.98); box-shadow: 0 2px 6px -1px rgba(15, 23, 42, 0.04); }");
  html += F(".card.navigating { pointer-events: none; opacity: 0.6; transform: scale(0.96); }");
  html += F(".card-content { display: flex; flex-direction: column; gap: 4px; }");
  html += F(".locked-card { opacity: 0.55 !important; background: var(--card-hover) !important; cursor: not-allowed !important; }");
  html += F(".program-btn.active { border-color: var(--primary); animation: activePulse 2s ease-in-out infinite; }");
  html += F("@keyframes activePulse { 0%, 100% { box-shadow: 0 0 0 0 rgba(37, 99, 235, 0.4); } 50% { box-shadow: 0 0 0 10px rgba(37, 99, 235, 0); } }");
  html += F(".title { font-size: 1.2rem; font-weight: 700; color: var(--text-main); display: flex; align-items: center; gap: 10px; }");
  html += F(".sub { color: var(--text-sub); font-size: 0.9rem; font-weight: 500; }");
  html += F(".action-icon { width: 38px; height: 38px; border-radius: 12px; background: var(--bg-color); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; color: var(--text-sub); transition: all 0.3s var(--cubic-smooth); flex-shrink: 0; }");
  html += F(".card:hover:not(.locked-card):not(.navigating) .action-icon { background: var(--primary); border-color: var(--primary); color: #ffffff; transform: translateX(4px); }");
  html += F(".stop-btn { margin: 20px auto 0; width: calc(100% - 32px); padding: 18px; border-radius: 18px; background: var(--danger); color: white; font-size: 1.25rem; font-weight: 800; text-align: center; cursor: pointer; display: none; box-shadow: 0 10px 25px -4px rgba(239, 68, 68, 0.45); transition: transform 0.3s var(--cubic-smooth); animation: stopPop 0.4s var(--cubic-smooth) forwards; }");
  html += F("@keyframes stopPop { from { opacity: 0; transform: translateY(10px) scale(0.95); } to { opacity: 1; transform: translateY(0) scale(1); } }");
  html += F(".stop-btn:active { transform: translateY(1px) scale(0.97); }");
  html += F(".overlay { position: fixed; inset: 0; background: rgba(15, 23, 42, 0.6); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); display: flex; align-items: center; justify-content: center; z-index: 100; padding: 20px; opacity: 0; pointer-events: none; transition: opacity 0.3s var(--cubic-smooth); }");
  html += F(".overlay.active { opacity: 1; pointer-events: auto; }");
  html += F(".modal { background: var(--card-bg); border-radius: 24px; padding: 28px; width: 100%; max-width: 340px; text-align: center; box-shadow: 0 20px 30px -10px rgba(0, 0, 0, 0.3); border: 1px solid var(--border); transform: scale(0.92) translateY(10px); transition: transform 0.3s var(--cubic-smooth); }");
  html += F(".overlay.active .modal { transform: scale(1) translateY(0); }");
  html += F(".modal h2 { font-size: 1.25rem; font-weight: 800; margin-bottom: 20px; }");
  html += F(".changelog-modal { max-width: 380px; text-align: left; }");
  html += F(".changelog-list { display: flex; flex-direction: column; gap: 12px; margin: 16px 0 24px; max-height: 240px; overflow-y: auto; padding-right: 4px; }");
  html += F(".changelog-item { display: flex; align-items: flex-start; gap: 10px; font-size: 0.9rem; font-weight: 500; color: var(--text-sub); }");
  html += F(".changelog-item span { color: var(--primary); font-weight: 700; flex-shrink: 0; }");
  html += F(".btn { width: 100%; padding: 15px; border-radius: 14px; font-size: 0.95rem; font-weight: 700; cursor: pointer; border: none; margin-top: 10px; transition: all 0.3s var(--cubic-smooth); }");
  html += F(".btn.primary { background: var(--primary); color: white; }");
  html += F(".btn.danger { background: var(--danger); color: white; }");
  html += F(".btn.cancel { background: var(--card-hover); color: var(--text-main); }");
  html += F(".btn:active { transform: scale(0.97); }");
  html += F(".system-info { margin: 24px auto 0; padding: 0 16px; text-align: center; transition: opacity 0.3s var(--cubic-smooth), transform 0.3s var(--cubic-smooth); opacity: 0; pointer-events: none; transform: translateY(14px); }");
  html += F(".system-info.visible { opacity: 1; pointer-events: auto; transform: translateY(0); }");
  html += F(".system-info-box { background: var(--card-bg); padding: 12px 20px; border-radius: 18px; border: 1px solid var(--border); font-size: 0.85rem; font-weight: 600; color: var(--text-sub); display: inline-flex; align-items: center; gap: 10px; transition: all 0.3s var(--cubic-smooth); }");
  html += F(".system-info-box.offline { border-color: var(--danger); color: var(--danger); background: rgba(239, 68, 68, 0.1); animation: offlinePulse 1.5s infinite; }");
  html += F("@keyframes offlinePulse { 0%, 100% { box-shadow: 0 0 0 0 rgba(239, 68, 68, 0.4); } 50% { box-shadow: 0 0 0 8px rgba(239, 68, 68, 0); } }");
  html += F("@keyframes fadeUp { from { opacity: 0; transform: translateY(14px); } to { opacity: 1; transform: translateY(0); } }");
  html += F(".fadeOut { opacity: 0 !important; transform: translateY(-10px) scale(0.96) !important; }");
  html += F("</style><script>");
  html += F("document.addEventListener('contextmenu', e => e.preventDefault());");
  html += F("let programRunning = false, isNavigating = false, currentId = null, pressTimer, delCardId = null, failCount = 0;");
  html += F("function triggerFeedback() { try { const ctx = new (window.AudioContext || window.webkitAudioContext)(); const o = ctx.createOscillator(), g = ctx.createGain(); o.type = 'sine'; o.frequency.setValueAtTime(550, ctx.currentTime); o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08); g.gain.setValueAtTime(0.06, ctx.currentTime); g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08); o.connect(g); g.connect(ctx.destination); o.start(); setTimeout(() => { o.stop(); ctx.close(); }, 90); } catch(e) {} try { if (navigator.vibrate) navigator.vibrate(40); } catch(e) {} }");
  html += F("function goUrl(url, el) { if (isNavigating) return; isNavigating = true; triggerFeedback(); if (el) el.classList.add('navigating'); document.querySelector('.container').classList.add('fadeOut'); setTimeout(() => location.href = url, 300); }");
  html += F("function runProgram(url, id) { if (programRunning || isNavigating) return; triggerFeedback(); programRunning = true; currentId = id; lockButtons(true); fetch(url).catch(()=>{}); }");
  html += F("function stopOnly() { triggerFeedback(); fetch('/STOP').catch(()=>{}); programRunning = false; currentId = null; lockButtons(false); }");
  html += F("function lockButtons(state) { document.querySelectorAll('.program-btn').forEach(b => b.classList.toggle('locked-card', state)); }");
  html += F("function updateSystemStatus(isOnline, textOffline) { const w = document.getElementById('sysInfoWrapper'), b = document.getElementById('sysStatusBox'); if (!isOnline) { w.classList.add('visible'); b.classList.add('offline'); b.innerHTML = '⚠️ ' + textOffline; lockButtons(true); } else { w.classList.remove('visible'); } }");
  html += F("async function upd() { if (isNavigating) return; try { const ctrl = new AbortController(); const tId = setTimeout(() => ctrl.abort(), 1500); let r = await fetch('/STATUS', { signal: ctrl.signal }); clearTimeout(tId); if (!r.ok) throw new Error(); let t = (await r.text()).trim(); failCount = 0; document.querySelectorAll('.program-btn').forEach(b => b.classList.remove('active')); if (t.startsWith('RUNNING')) { programRunning = true; lockButtons(true); let id = t.split(':')[1]; let btn = document.querySelector('.program-btn[data-id=\"' + id + '\"]'); if (btn) btn.classList.add('active'); document.getElementById('stopBtn').style.display = 'block'; } else { programRunning = false; lockButtons(false); document.getElementById('stopBtn').style.display = 'none'; } updateSystemStatus(true, document.getElementById('sysStatusBox').dataset.offline); } catch(e) { if (++failCount >= 3) { updateSystemStatus(false, document.getElementById('sysStatusBox').dataset.offline); } } }");
  html += F("function lpStart(id) { pressTimer = setTimeout(() => { triggerFeedback(); if (navigator.vibrate) navigator.vibrate([30, 50, 30]); delCardId = id; document.getElementById('overlay').classList.add('active'); }, 700); }");
  html += F("function lpEnd() { clearTimeout(pressTimer); }");
  html += F("function delCard() { triggerFeedback(); fetch('/DELCARD?id=' + delCardId).then(() => location.reload()).catch(()=>{}); }");
  html += F("function closeOverlay() { triggerFeedback(); document.getElementById('overlay').classList.remove('active'); }");
  html += F("function closeChangelog() { triggerFeedback(); document.getElementById('changelogOverlay').classList.remove('active'); localStorage.setItem('targetix_changelog_seen', 'VER_1_0'); }");
  html += F("window.addEventListener('DOMContentLoaded', () => { if (localStorage.getItem('targetix_changelog_seen') !== 'VER_1_0') { document.getElementById('changelogOverlay').classList.add('active'); } });");
  html += F("setInterval(upd, 500);");
  html += F("</script></head><body>");
  html += F("<div class='container'><header><div class='header-title'>TargetiX</div></header><div class='grid'>");

  // Настройки
  html += F("<div class='card' onclick=\"goUrl('/SETTINGS', this)\"><div class='card-content'><div class='title'>⚙️ ");
  html += tr("Einstellungen", "Налаштування", "Settings");
  html += F("</div><div class='sub'>");
  html += tr("Systemparameter", "Параметри системи", "System config");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M9 18l6-6-6-6'/></svg></div></div>");

  // Пресет MP5
  html += F("<div class='card program-btn' data-id='MP5' onclick=\"runProgram('/RUNPRESET?mode=MP5','MP5')\"><div class='card-content'><div class='title'>🎯 ");
  html += modeName("MP5");
  html += F("</div><div class='sub'>");
  html += tr("Schnellstart", "Швидкий запуск", "Quick start");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  // Пресет MP8
  html += F("<div class='card' onclick=\"goUrl('/MP8', this)\"><div class='card-content'><div class='title'>🎯 ");
  html += modeName("MP8");
  html += F("</div><div class='sub'>");
  html += tr("Profilauswahl", "Вибір профілю", "Select profile");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M9 18l6-6-6-6'/></svg></div></div>");

  // Пресет MP10
  html += F("<div class='card' onclick=\"goUrl('/MP10', this)\"><div class='card-content'><div class='title'>🎯 ");
  html += modeName("MP10");
  html += F("</div><div class='sub'>");
  html += tr("Profilauswahl", "Вибір профілю", "Select profile");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M9 18l6-6-6-6'/></svg></div></div>");

  // Пользовательские карточки
  for (uint8_t i = 0; i < cardCount; i++) {
    html += F("<div class='card' onmousedown='lpStart(");
    html += String(i);
    html += F(")' onmouseup='lpEnd()' ontouchstart='lpStart(");
    html += String(i);
    html += F(")' ontouchend='lpEnd()' onclick=\"goUrl('/CARD?id=");
    html += String(i);
    html += F("', this)\"><div class='card-content'><div class='title'>📁 ");
    html += String(cards[i].title);
    html += F("</div><div class='sub'>");
    html += tr("Programme: ", "Програми: ", "Programs: ");
    html += String(cards[i].count);
    html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M9 18l6-6-6-6'/></svg></div></div>");
  }

  // Добавление новых карточек
  html += F("<div class='card' onclick=\"goUrl('/ADD', this)\"><div class='card-content'><div class='title'>➕ ");
  html += tr("Hinzufügen", "Додати", "Add");
  html += F("</div><div class='sub'>");
  html += tr("Neues Programm", "Нова програма", "New program");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M9 18l6-6-6-6'/></svg></div></div>");
  
  html += F("</div><div id='stopBtn' class='stop-btn' onclick='stopOnly()'>⛔ ");
  html += tr("STOPP", "СТОП", "STOP");
  html += F("</div><div id='sysInfoWrapper' class='system-info'><div id='sysStatusBox' class='system-info-box' data-offline='");
  html += tr("Verbindung unterbrochen", "Зв'язок втрачено", "Connection Lost");
  html += F("'></div></div></div>");

  // Окно Что нового (Changelog)
  html += F("<div id='changelogOverlay' class='overlay'><div class='modal changelog-modal'><h2>🚀 ");
  html += tr("Was ist neu?", "Що нового?", "What's new?");
  html += F("</h2><div class='changelog-list'>");
  html += F("<div class='changelog-item'><span>✨</span><div>");
  html += tr("Modernisiertes Interface mit flüssigen Animationen und haptischem Feedback.", "Модернізований інтерфейс з плавними анімаціями та тактильним відгуком.", "Modernized interface with smooth animations and haptic feedback.");
  html += F("</div></div>");
  html += F("<div class='changelog-item'><span>⚡</span><div>");
  html += tr("Deutliche Optimierung der Verbindungsstabilität und des Status-Pollings.", "Значна оптимізація стабільності з'єднання та опитування статусу.", "Significant optimization of connection stability and status polling.");
  html += F("</div></div>");
  html += F("<div class='changelog-item'><span>🛡️</span><div>");
  html += tr("Verbesserte Systemfehlererkennung und verbesserter Dunkelmodus.", "Покращено виявлення системних збоїв та вдосконалено темний режим.", "Improved system error detection and enhanced dark mode.");
  html += F("</div></div>");
  html += F("</div><button class='btn primary' onclick='closeChangelog()'>");
  html += tr("Verstanden", "Зрозуміло", "Got it");
  html += F("</button></div></div>");

  // Модальное окно удаления карточки
  html += F("<div id='overlay' class='overlay'><div class='modal'><h2>");
  html += tr("Karte löschen?", "Видалити картку?", "Delete card?");
  html += F("</h2><button class='btn danger' onclick='delCard()'>");
  html += tr("Löschen", "Видалити", "Delete");
  html += F("</button><button class='btn cancel' onclick='closeOverlay()'>");
  html += tr("Abbrechen", "Скасувати", "Cancel");
  html += F("</button></div></div></body></html>");

  return html;
}

String mp8Page() {
  String html;
  html.reserve(10000);

  html += F("<!DOCTYPE html><html lang='ru'><head>");
  html += F("<meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>");
  html += F("<title>TargetiX - МП8</title>");
  html += F("<style>");
  html += F(":root { --bg-color: #f8fafc; --card-bg: #ffffff; --card-hover: #f1f5f9; --text-main: #0f172a; --text-sub: #64748b; --primary: #2563eb; --primary-hover: #1d4ed8; --border: #e2e8f0; --danger: #ef4444; --danger-hover: #dc2626; --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1); }");
  html += F("@media (prefers-color-scheme: dark) { :root { --bg-color: #0f172a; --card-bg: #1e293b; --card-hover: #334155; --text-main: #f8fafc; --text-sub: #94a3b8; --border: #334155; } }");
  html += F("* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; user-select: none; }");
  html += F("body { min-height: 100vh; background-color: var(--bg-color); background-image: radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%), radial-gradient(var(--border) 1px, transparent 1px); background-size: 100% 100%, 24px 24px; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; color: var(--text-main); padding-bottom: 40px; overflow-x: hidden; animation: ambientBreath 8s ease-in-out infinite alternate; }");
  html += F("@keyframes ambientBreath { 0% { background-position: 0% 0%, 0 0; } 100% { background-position: 0% 0%, 12px 12px; } }");
  html += F(".container { max-width: 440px; margin: 0 auto; transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth); }");
  html += F("header { padding: 24px 20px 14px; display: flex; align-items: center; justify-content: space-between; opacity: 0; animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards; }");
  html += F(".back-btn { width: 44px; height: 44px; border-radius: 14px; background: var(--card-bg); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; color: var(--text-main); text-decoration: none; box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03); transition: all 0.3s var(--cubic-smooth); }");
  html += F(".back-btn:hover { background: var(--card-hover); border-color: var(--text-sub); transform: translateY(-1px); }");
  html += F(".back-btn:active { transform: scale(0.92); }");
  html += F(".header-title { font-size: 1.5rem; font-weight: 800; letter-spacing: -0.02em; color: var(--text-main); }");
  html += F(".grid { padding: 12px 16px; display: flex; flex-direction: column; gap: 14px; width: 100%; }");
  html += F(".card { background: var(--card-bg); padding: 20px 22px; border-radius: 22px; border: 1px solid var(--border); box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03), 0 2px 4px -2px rgba(15, 23, 42, 0.02); display: flex; align-items: center; justify-content: space-between; cursor: pointer; position: relative; opacity: 0; transition: transform 0.3s var(--cubic-smooth), box-shadow 0.3s var(--cubic-smooth), border-color 0.3s var(--cubic-smooth), background-color 0.3s var(--cubic-smooth), opacity 0.3s var(--cubic-smooth); }");
  html += F(".card:nth-child(1) { animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards; }");
  html += F(".card:nth-child(2) { animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards; }");
  html += F(".card:nth-child(3) { animation: fadeUp 0.6s var(--cubic-smooth) 0.4s forwards; }");
  html += F(".card:hover { transform: translateY(-2px); box-shadow: 0 12px 22px -4px rgba(15, 23, 42, 0.08); border-color: var(--text-sub); }");
  html += F(".card:active { transform: translateY(1px) scale(0.98); box-shadow: 0 2px 6px -1px rgba(15, 23, 42, 0.04); }");
  html += F(".card.locked { opacity: 0.55 !important; background: var(--card-hover) !important; cursor: not-allowed !important; pointer-events: none; border-color: var(--border) !important; box-shadow: none !important; transform: none !important; }");
  html += F(".card-content { display: flex; flex-direction: column; gap: 4px; }");
  html += F(".title { font-size: 1.2rem; font-weight: 700; color: var(--text-main); display: flex; align-items: center; gap: 10px; }");
  html += F(".sub { color: var(--text-sub); font-size: 0.9rem; font-weight: 500; }");
  html += F(".action-icon { width: 38px; height: 38px; border-radius: 12px; background: var(--bg-color); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; color: var(--primary); transition: all 0.3s var(--cubic-smooth); flex-shrink: 0; }");
  html += F(".card:hover .action-icon { background: var(--primary); border-color: var(--primary); color: #ffffff; transform: scale(1.05); }");
  html += F(".stop-btn { margin: 20px auto 0; width: calc(100% - 32px); padding: 18px; border-radius: 18px; background: var(--danger); color: white; font-size: 1.25rem; font-weight: 800; text-align: center; cursor: pointer; display: none; box-shadow: 0 10px 25px -4px rgba(239, 68, 68, 0.45); transition: transform 0.3s var(--cubic-smooth), box-shadow 0.3s var(--cubic-smooth), background-color 0.3s var(--cubic-smooth); animation: stopPop 0.4s var(--cubic-smooth) forwards; }");
  html += F("@keyframes stopPop { from { opacity: 0; transform: translateY(10px) scale(0.95); } to { opacity: 1; transform: translateY(0) scale(1); } }");
  html += F(".stop-btn:hover { background: var(--danger-hover); transform: translateY(-2px); box-shadow: 0 14px 28px -4px rgba(239, 68, 68, 0.55); }");
  html += F(".stop-btn:active { transform: translateY(1px) scale(0.97); }");
  html += F("@keyframes fadeUp { from { opacity: 0; transform: translateY(14px); } to { opacity: 1; transform: translateY(0); } }");
  html += F(".fadeOut { opacity: 0 !important; transform: translateY(-10px) scale(0.96) !important; }");
  html += F("</style><script>");
  html += F("document.addEventListener('contextmenu', e => e.preventDefault());");
  html += F("document.addEventListener('copy', e => e.preventDefault());");
  html += F("document.addEventListener('cut', e => e.preventDefault());");
  html += F("let programRunning = false;");
  html += F("function clickSound() { try { const ctx = new (window.AudioContext || window.webkitAudioContext)(); const o = ctx.createOscillator(); const g = ctx.createGain(); o.type = 'sine'; o.frequency.setValueAtTime(550, ctx.currentTime); o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08); g.gain.setValueAtTime(0.06, ctx.currentTime); g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08); o.connect(g); g.connect(ctx.destination); o.start(); setTimeout(() => { o.stop(); ctx.close(); }, 90); } catch(e) {} try { if (navigator.vibrate) navigator.vibrate(40); } catch(e) {} }");
  html += F("function goUrl(url) { clickSound(); const container = document.querySelector('.container'); container.classList.add('fadeOut'); setTimeout(() => { location.href = url; }, 300); }");
  html += F("function lockCards(state) { document.querySelectorAll('.card').forEach(c => { c.classList.toggle('locked', state); }); }");
  html += F("function runMP8(url) { clickSound(); if (programRunning) return; programRunning = true; lockCards(true); fetch(url); }");
  html += F("function stopOnly() { clickSound(); fetch('/STOP'); programRunning = false; lockCards(false); }");
  html += F("async function upd() { try { let r = await fetch('/STATUS'); let t = (await r.text()).trim(); if (t.startsWith('RUNNING')) { programRunning = true; lockCards(true); document.getElementById('stopBtn').style.display = 'block'; } else { programRunning = false; lockCards(false); document.getElementById('stopBtn').style.display = 'none'; } } catch(e) {} }");
  html += F("setInterval(upd, 500); window.onload = upd;");
  html += F("</script></head><body>");
  html += F("<div class='container'><header><a href='javascript:void(0)' onclick=\"goUrl('/')\" class='back-btn'><svg width='22' height='22' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M19 12H5'/><path d='M12 19l-7-7 7-7'/></svg></a><div class='header-title'>");
  
  html += modeName("MP8");
  
  html += F("</div><div style='width: 44px;'></div></header><div class='grid'>");

  // Карточка 1 (8 секунд)
  html += F("<div class='card' onclick=\"runMP8('/RUNPRESET?mode=MP8&g=8')\"><div class='card-content'><div class='title'>⏱️ ");
  html += tr("8 Sekunden", "8 секунд", "8 seconds");
  html += F("</div><div class='sub'>");
  html += tr("Schussserie", "Серія пострілів", "Shot series");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  // Карточка 2 (6 секунд)
  html += F("<div class='card' onclick=\"runMP8('/RUNPRESET?mode=MP8&g=6')\"><div class='card-content'><div class='title'>⏱️ ");
  html += tr("6 Sekunden", "6 секунд", "6 seconds");
  html += F("</div><div class='sub'>");
  html += tr("Schussserie", "Серія пострілів", "Shot series");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  // Карточка 3 (4 секунды)
  html += F("<div class='card' onclick=\"runMP8('/RUNPRESET?mode=MP8&g=4')\"><div class='card-content'><div class='title'>⏱️ ");
  html += tr("4 Sekunden", "4 секунди", "4 seconds");
  html += F("</div><div class='sub'>");
  html += tr("Schussserie", "Серія пострілів", "Shot series");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  html += F("</div>");
  html += F("<div id='stopBtn' class='stop-btn' onclick='stopOnly()'>⛔ ");
  html += tr("STOPP", "СТОП", "STOP");
  html += F("</div></div></body></html>");

  return html;
}
String mp10Page() {
  String html;
  html.reserve(10000);

  html += F("<!DOCTYPE html><html lang='ru'><head>");
  html += F("<meta charset='UTF-8'>");
  html += F("<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>");
  html += F("<title>TargetiX - МП10</title>");
  html += F("<style>");
  html += F(":root { --bg-color: #f8fafc; --card-bg: #ffffff; --card-hover: #f1f5f9; --text-main: #0f172a; --text-sub: #64748b; --primary: #2563eb; --primary-hover: #1d4ed8; --border: #e2e8f0; --danger: #ef4444; --danger-hover: #dc2626; --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1); }");
  html += F("@media (prefers-color-scheme: dark) { :root { --bg-color: #0f172a; --card-bg: #1e293b; --card-hover: #334155; --text-main: #f8fafc; --text-sub: #94a3b8; --border: #334155; } }");
  html += F("* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; user-select: none; }");
  html += F("body { min-height: 100vh; background-color: var(--bg-color); background-image: radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%), radial-gradient(var(--border) 1px, transparent 1px); background-size: 100% 100%, 24px 24px; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; color: var(--text-main); padding-bottom: 40px; overflow-x: hidden; animation: ambientBreath 8s ease-in-out infinite alternate; }");
  html += F("@keyframes ambientBreath { 0% { background-position: 0% 0%, 0 0; } 100% { background-position: 0% 0%, 12px 12px; } }");
  html += F(".container { max-width: 440px; margin: 0 auto; transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth); }");
  html += F("header { padding: 24px 20px 14px; display: flex; align-items: center; justify-content: space-between; opacity: 0; animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards; }");
  html += F(".back-btn { width: 44px; height: 44px; border-radius: 14px; background: var(--card-bg); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; color: var(--text-main); text-decoration: none; box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03); transition: all 0.3s var(--cubic-smooth); }");
  html += F(".back-btn:hover { background: var(--card-hover); border-color: var(--text-sub); transform: translateY(-1px); }");
  html += F(".back-btn:active { transform: scale(0.92); }");
  html += F(".header-title { font-size: 1.5rem; font-weight: 800; letter-spacing: -0.02em; color: var(--text-main); }");
  html += F(".grid { padding: 12px 16px; display: flex; flex-direction: column; gap: 14px; width: 100%; }");
  html += F(".card { background: var(--card-bg); padding: 20px 22px; border-radius: 22px; border: 1px solid var(--border); box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03), 0 2px 4px -2px rgba(15, 23, 42, 0.02); display: flex; align-items: center; justify-content: space-between; cursor: pointer; position: relative; opacity: 0; transition: transform 0.3s var(--cubic-smooth), box-shadow 0.3s var(--cubic-smooth), border-color 0.3s var(--cubic-smooth), background-color 0.3s var(--cubic-smooth), opacity 0.3s var(--cubic-smooth); }");
  html += F(".card:nth-child(1) { animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards; }");
  html += F(".card:nth-child(2) { animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards; }");
  html += F(".card:nth-child(3) { animation: fadeUp 0.6s var(--cubic-smooth) 0.4s forwards; }");
  html += F(".card:hover { transform: translateY(-2px); box-shadow: 0 12px 22px -4px rgba(15, 23, 42, 0.08); border-color: var(--text-sub); }");
  html += F(".card:active { transform: translateY(1px) scale(0.98); box-shadow: 0 2px 6px -1px rgba(15, 23, 42, 0.04); }");
  html += F(".card.locked { opacity: 0.55 !important; background: var(--card-hover) !important; cursor: not-allowed !important; pointer-events: none; border-color: var(--border) !important; box-shadow: none !important; transform: none !important; }");
  html += F(".card-content { display: flex; flex-direction: column; gap: 4px; }");
  html += F(".title { font-size: 1.2rem; font-weight: 700; color: var(--text-main); display: flex; align-items: center; gap: 10px; }");
  html += F(".sub { color: var(--text-sub); font-size: 0.9rem; font-weight: 500; }");
  html += F(".action-icon { width: 38px; height: 38px; border-radius: 12px; background: var(--bg-color); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; color: var(--primary); transition: all 0.3s var(--cubic-smooth); flex-shrink: 0; }");
  html += F(".card:hover .action-icon { background: var(--primary); border-color: var(--primary); color: #ffffff; transform: scale(1.05); }");
  html += F(".stop-btn { margin: 20px auto 0; width: calc(100% - 32px); padding: 18px; border-radius: 18px; background: var(--danger); color: white; font-size: 1.25rem; font-weight: 800; text-align: center; cursor: pointer; display: none; box-shadow: 0 10px 25px -4px rgba(239, 68, 68, 0.45); transition: transform 0.3s var(--cubic-smooth), box-shadow 0.3s var(--cubic-smooth), background-color 0.3s var(--cubic-smooth); animation: stopPop 0.4s var(--cubic-smooth) forwards; }");
  html += F("@keyframes stopPop { from { opacity: 0; transform: translateY(10px) scale(0.95); } to { opacity: 1; transform: translateY(0) scale(1); } }");
  html += F(".stop-btn:hover { background: var(--danger-hover); transform: translateY(-2px); box-shadow: 0 14px 28px -4px rgba(239, 68, 68, 0.55); }");
  html += F(".stop-btn:active { transform: translateY(1px) scale(0.97); }");
  html += F("@keyframes fadeUp { from { opacity: 0; transform: translateY(14px); } to { opacity: 1; transform: translateY(0); } }");
  html += F(".fadeOut { opacity: 0 !important; transform: translateY(-10px) scale(0.96) !important; }");
  html += F("</style><script>");
  html += F("document.addEventListener('contextmenu', e => e.preventDefault());");
  html += F("document.addEventListener('copy', e => e.preventDefault());");
  html += F("document.addEventListener('cut', e => e.preventDefault());");
  html += F("let programRunning = false;");
  html += F("function clickSound() { try { const ctx = new (window.AudioContext || window.webkitAudioContext)(); const o = ctx.createOscillator(); const g = ctx.createGain(); o.type = 'sine'; o.frequency.setValueAtTime(550, ctx.currentTime); o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08); g.gain.setValueAtTime(0.06, ctx.currentTime); g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08); o.connect(g); g.connect(ctx.destination); o.start(); setTimeout(() => { o.stop(); ctx.close(); }, 90); } catch(e) {} try { if (navigator.vibrate) navigator.vibrate(40); } catch(e) {} }");
  html += F("function goUrl(url) { clickSound(); const container = document.querySelector('.container'); container.classList.add('fadeOut'); setTimeout(() => { location.href = url; }, 300); }");
  html += F("function lockCards(state) { document.querySelectorAll('.card').forEach(c => { c.classList.toggle('locked', state); }); }");
  html += F("function runMP10(url) { clickSound(); if (programRunning) return; programRunning = true; lockCards(true); fetch(url); }");
  html += F("function stopOnly() { clickSound(); fetch('/STOP'); programRunning = false; lockCards(false); }");
  html += F("async function upd() { try { let r = await fetch('/STATUS'); let t = (await r.text()).trim(); if (t.startsWith('RUNNING')) { programRunning = true; lockCards(true); document.getElementById('stopBtn').style.display = 'block'; } else { programRunning = false; lockCards(false); document.getElementById('stopBtn').style.display = 'none'; } } catch(e) {} }");
  html += F("setInterval(upd, 500); window.onload = upd;");
  html += F("</script></head><body>");
  html += F("<div class='container'><header><a href='javascript:void(0)' onclick=\"goUrl('/')\" class='back-btn'><svg width='22' height='22' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><path d='M19 12H5'/><path d='M12 19l-7-7 7-7'/></svg></a><div class='header-title'>");
  
  html += modeName("MP10");
  
  html += F("</div><div style='width: 44px;'></div></header><div class='grid'>");

  // Карточка 1 (150 секунд)
  html += F("<div class='card' onclick=\"runMP10('/RUNPRESET?mode=MP10&g=150')\"><div class='card-content'><div class='title'>⏱️ ");
  html += tr("150 Sekunden", "150 секунд", "150 seconds");
  html += F("</div><div class='sub'>");
  html += tr("Schussserie", "Серія пострілів", "Shot series");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  // Карточка 2 (20 секунд)
  html += F("<div class='card' onclick=\"runMP10('/RUNPRESET?mode=MP10&g=20')\"><div class='card-content'><div class='title'>⏱️ ");
  html += tr("20 Sekunden", "20 секунд", "20 seconds");
  html += F("</div><div class='sub'>");
  html += tr("Schussserie", "Серія пострілів", "Shot series");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  // Карточка 3 (10 секунд)
  html += F("<div class='card' onclick=\"runMP10('/RUNPRESET?mode=MP10&g=10')\"><div class='card-content'><div class='title'>⏱️ ");
  html += tr("10 Sekunden", "10 секунд", "10 seconds");
  html += F("</div><div class='sub'>");
  html += tr("Schussserie", "Серія пострілів", "Shot series");
  html += F("</div></div><div class='action-icon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'><polygon points='5 3 19 12 5 21 5 3'/></svg></div></div>");

  html += F("</div>");
  html += F("<div id='stopBtn' class='stop-btn' onclick='stopOnly()'>⛔ ");
  html += tr("STOPP", "СТОП", "STOP");
  html += F("</div></div></body></html>");

  return html;
}

String addPage() {
  String html;
  html.reserve(4500);

  html += R"====es(<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>TargetiX — Новая программа</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --border: #e2e8f0;
      --border-focus: #93c5fd;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    /* Автоматическая тёмная тема */
    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    /* Разрешаем выделение текста внутри полей ввода */
    input, select {
      user-select: text;
      -webkit-user-select: text;
    }

    body {
      min-height: 100vh;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      color: var(--text-main);
      padding-bottom: 40px;
      overflow-x: hidden;
      animation: ambientBreath 8s ease-in-out infinite alternate;
    }

    @keyframes ambientBreath {
      0% { background-position: 0% 0%, 0 0; }
      100% { background-position: 0% 0%, 12px 12px; }
    }

    .container {
      max-width: 440px;
      margin: 0 auto;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
    }

    /* Шапка */
    header {
      padding: 24px 20px 14px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards;
    }

    .back-btn {
      width: 44px;
      height: 44px;
      border-radius: 14px;
      background: var(--card-bg);
      border: 1px solid var(--border);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--text-main);
      text-decoration: none;
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03);
      transition: all 0.3s var(--cubic-smooth);
      cursor: pointer;
    }

    .back-btn:hover {
      background: var(--card-hover);
      border-color: var(--text-sub);
      transform: translateY(-1px);
    }

    .back-btn:active {
      transform: scale(0.92);
    }

    .header-title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
    }

    /* Форма */
    form {
      padding: 12px 16px;
      display: flex;
      flex-direction: column;
      gap: 16px;
      width: 100%;
    }

    .form-group {
      display: flex;
      flex-direction: column;
      gap: 6px;
      opacity: 0;
    }

    /* Каскадная анимация полей */
    .form-group:nth-child(1) { animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards; }
    .form-group:nth-child(2) { animation: fadeUp 0.6s var(--cubic-smooth) 0.25s forwards; }
    .form-group:nth-child(3) { animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards; }
    .form-group:nth-child(4) { animation: fadeUp 0.6s var(--cubic-smooth) 0.35s forwards; }
    .form-group:nth-child(5) { animation: fadeUp 0.6s var(--cubic-smooth) 0.4s forwards; }
    .form-group:nth-child(6) { animation: fadeUp 0.6s var(--cubic-smooth) 0.45s forwards; }
    .submit-wrap { opacity: 0; animation: fadeUp 0.6s var(--cubic-smooth) 0.5s forwards; }

    label {
      font-size: 0.85rem;
      font-weight: 700;
      color: var(--text-sub);
      text-transform: uppercase;
      letter-spacing: 0.04em;
      margin-left: 4px;
    }

    .input-wrapper {
      position: relative;
      display: flex;
      align-items: center;
    }

    .input-icon {
      position: absolute;
      left: 16px;
      color: var(--text-sub);
      pointer-events: none;
      display: flex;
      align-items: center;
      justify-content: center;
    }

    input, select {
      width: 100%;
      padding: 16px 16px 16px 48px;
      font-size: 1rem;
      font-weight: 600;
      color: var(--text-main);
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 18px;
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03);
      outline: none;
      appearance: none;
      -webkit-appearance: none;
      transition: all 0.25s var(--cubic-smooth);
    }

    select {
      cursor: pointer;
      background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='%2364748b' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpolyline points='6 9 12 15 18 9'%3E%3C/polyline%3E%3C/svg%3E");
      background-repeat: no-repeat;
      background-position: right 16px center;
      padding-right: 44px;
    }

    select option {
      background-color: var(--card-bg);
      color: var(--text-main);
    }

    input:focus, select:focus {
      border-color: var(--primary);
      box-shadow: 0 0 0 4px rgba(37, 99, 235, 0.12), 0 8px 16px -4px rgba(15, 23, 42, 0.06);
      background: var(--card-bg);
    }

    /* Специфический контейнер для динамического поля новой карточки */
    #newCardGroup {
      display: block;
      transition: all 0.3s var(--cubic-smooth);
    }

    #newCardGroup.hidden {
      display: none;
    }

    /* Кнопка сохранить */
    .save-btn {
      margin-top: 8px;
      width: 100%;
      padding: 18px;
      border-radius: 18px;
      border: none;
      background: var(--primary);
      color: white;
      font-size: 1.15rem;
      font-weight: 800;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 10px;
      box-shadow: 0 10px 25px -4px rgba(37, 99, 235, 0.4);
      transition: all 0.3s var(--cubic-smooth);
    }

    .save-btn:hover {
      background: var(--primary-hover);
      transform: translateY(-2px);
      box-shadow: 0 14px 28px -4px rgba(37, 99, 235, 0.5);
    }

    .save-btn:active {
      transform: translateY(1px) scale(0.98);
    }

    /* Анимация появления */
    @keyframes fadeUp {
      from {
        opacity: 0;
        transform: translateY(14px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    /* Плавный уход */
    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());
    document.addEventListener('copy', e => e.preventDefault());
    document.addEventListener('cut', e => e.preventDefault());

    function clickSound() {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(550, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    }

    function goBack() {
      clickSound();
      const container = document.querySelector('.container');
      container.classList.add('fadeOut');
      setTimeout(() => {
        history.back();
      }, 300);
    }

    function onCardChange(sel) {
      clickSound();
      let grp = document.getElementById("newCardGroup");
      let input = document.querySelector("input[name='newcard']");
      
      if(sel.value === "") {
        grp.classList.remove("hidden");
        input.required = true;
      } else {
        grp.classList.add("hidden");
        input.required = false;
        input.value = "";
      }
    }
  </script>
</head>
<body>

  <div class="container">
    <header>
      <div onclick="goBack()" class="back-btn">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M19 12H5"/><path d="M12 19l-7-7 7-7"/></svg>
      </div>
      <div class="header-title">)====es"
          + tr("Neue Programm", "Нова програма", "New Program") + R"====es(</div>
      <div style="width: 44px;"></div> <!-- Заглушка для центрирования -->
    </header>

    <form action="/SAVE" method="get" onsubmit="clickSound()">

      <div class="form-group">
        <label>)====es"
          + tr("Programmname", "Назва програми", "Program Name") + R"====es(</label>
        <div class="input-wrapper">
          <div class="input-icon">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/></svg>
          </div>
          <input name="progname" placeholder=")====es"
          + tr("Beispiel: Schnellfeuer", "Наприклад: Швидка стрільба", "Example: Rapid fire") + R"====es(" maxlength="19" required>
        </div>
      </div>

      <div class="form-group">
        <label>)====es"
          + tr("Kategorie / Karte", "Категорія / Картка", "Category / Card") + R"====es(</label>
        <div class="input-wrapper">
          <div class="input-icon">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="9" y1="21" x2="9" y2="9"/></svg>
          </div>
          <select name="card" onchange="onCardChange(this)">
            <option value="">➕ )====es"
          + tr("Neue Karte", "Нова картка", "New Card") + R"====es(</option>
)====es";

  for (uint8_t i = 0; i < cardCount; i++) {
    html += "<option value='" + String(i) + "'>" + String(cards[i].title) + "</option>";
  }

  html += R"====es(</select>
        </div>
      </div>

      <div class="form-group" id="newCardGroup">
        <label>)====es"
          + tr("Name der neuen Karte", "Ім'я нової картки", "New card name") + R"====es(</label>
        <div class="input-wrapper">
          <div class="input-icon">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/><line x1="12" y1="11" x2="12" y2="17"/><line x1="9" y1="14" x2="15" y2="14"/></svg>
          </div>
          <input type="text" name="newcard" placeholder=")====es"
          + tr("Beispiel: Meine Trainings", "Наприклад: Мої тренування", "Example: My workouts") + R"====es(">
        </div>
      </div>

      <div class="form-group">
        <label>)====es"
          + tr("Rotes Signal (Sek.)", "Червоний сигнал (сек.)", "Red signal (sec)") + R"====es(</label>
        <div class="input-wrapper">
          <div class="input-icon" style="color: #ef4444;">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
          </div>
          <input type="number" name="red" placeholder=")====es"
          + tr("Pause vor dem Start", "Пауза перед стартом", "Pause before start") + R"====es(" min="1" required>
        </div>
      </div>

      <div class="form-group">
        <label>)====es"
          + tr("Grünes Signal (Sek.)", "Зелений сигнал (сек.)", "Green signal (sec)") + R"====es(</label>
        <div class="input-wrapper">
          <div class="input-icon" style="color: #22c55e;">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
          </div>
          <input type="number" name="green" placeholder=")====es"
          + tr("Schießzeit", "Час на стрільбу", "Shooting time") + R"====es(" min="1" required>
        </div>
      </div>

      <div class="form-group">
        <label>)====es"
          + tr("Anzahl der Serien", "Кількість серій", "Number of series") + R"====es(</label>
        <div class="input-wrapper">
          <div class="input-icon">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>
          </div>
          <input type="number" name="series" placeholder=")====es"
          + tr("Anzahl der Wiederholungen", "Кількість повторень", "Number of repetitions") + R"====es(" min="1" required>
        </div>
      </div>

      <div class="submit-wrap">
        <button type="submit" class="save-btn">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
          )====es"
          + tr("Programm speichern", "Зберегти програму", "Save program") + R"====es(
        </button>
      </div>

    </form>
  </div>

</body>
</html>)====es";

  return html;
}

String updatePage() {

  String status;
  String statusClass;

  if (remoteVersion == "") {
    status = "❌ " + tr("Nicht überprüft", "Не перевірено", "Not checked");
    statusClass = "status-none";
  } else if (updateAvailableFlag) {
    status = "🟡 " + tr("Update verfügbar", "Доступне оновлення", "Update available");
    statusClass = "status-available";
  } else {
    status = "🟢 " + tr("Sie haben die neueste Version", "У вас остання версія", "You have the latest version");
    statusClass = "status-ok";
  }

  String html;
  html.reserve(6000);

  html += R"=====(<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
  <meta http-equiv="Pragma" content="no-cache">
  <meta http-equiv="Expires" content="0">
  <title>TargetiX — Software-Update</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --danger: #ef4444;
      --danger-hover: #dc2626;
      --border: #e2e8f0;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    /* Автоматическая тёмная тема */
    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    body {
      min-height: 100vh;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      color: var(--text-main);
      padding: 20px 16px 40px;
      overflow-x: hidden;
      animation: ambientBreath 8s ease-in-out infinite alternate;
    }

    @keyframes ambientBreath {
      0% { background-position: 0% 0%, 0 0; }
      100% { background-position: 0% 0%, 12px 12px; }
    }

    .container {
      max-width: 440px;
      margin: 0 auto;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
    }

    /* Шапка */
    header {
      padding: 12px 4px 20px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards;
    }

    .back-btn {
      width: 44px;
      height: 44px;
      border-radius: 14px;
      background: var(--card-bg);
      border: 1px solid var(--border);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--text-main);
      text-decoration: none;
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03);
      transition: all 0.3s var(--cubic-smooth);
      cursor: pointer;
    }

    .back-btn:hover {
      background: var(--card-hover);
      border-color: var(--text-sub);
      transform: translateY(-1px);
    }

    .back-btn:active {
      transform: scale(0.92);
    }

    .header-title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
    }

    /* Главная карточка */
    .card {
      background: var(--card-bg);
      padding: 24px 22px;
      border-radius: 24px;
      border: 1px solid var(--border);
      box-shadow: 0 10px 25px -5px rgba(15, 23, 42, 0.03), 
                  0 4px 10px -2px rgba(15, 23, 42, 0.02);
      display: flex;
      flex-direction: column;
      gap: 16px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards;
    }

    /* Блок информации о версиях */
    .version-box {
      background: var(--bg-color);
      border: 1px solid var(--border);
      border-radius: 18px;
      padding: 16px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    .version-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 0.95rem;
      font-weight: 600;
      color: var(--text-sub);
    }

    .version-badge {
      font-weight: 700;
      color: var(--text-main);
      background: var(--card-bg);
      padding: 4px 10px;
      border-radius: 10px;
      border: 1px solid var(--border);
      font-family: monospace, sans-serif;
    }

    /* Статус проверки */
    .status-text {
      text-align: center;
      font-size: 1.1rem;
      font-weight: 800;
      padding: 10px;
      border-radius: 14px;
      background: var(--card-hover);
      color: var(--text-main);
      transition: all 0.3s ease;
    }

    /* Кнопки */
    .actions-grid {
      display: flex;
      flex-direction: column;
      gap: 12px;
      margin-top: 4px;
    }

    .btn {
      position: relative;
      overflow: hidden;
      width: 100%;
      padding: 16px;
      border-radius: 18px;
      border: none;
      background: var(--primary);
      color: white;
      font-size: 1.05rem;
      font-weight: 800;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      box-shadow: 0 8px 20px -4px rgba(37, 99, 235, 0.35);
      transition: transform 0.25s var(--cubic-smooth), 
                  box-shadow 0.25s var(--cubic-smooth), 
                  background-color 0.25s var(--cubic-smooth);
    }

    .btn:hover {
      background: var(--primary-hover);
      transform: translateY(-2px);
      box-shadow: 0 12px 24px -4px rgba(37, 99, 235, 0.45);
    }

    .btn:active {
      transform: translateY(1px) scale(0.98);
    }

    .btn.red {
      background: var(--danger);
      box-shadow: 0 8px 20px -4px rgba(239, 68, 68, 0.35);
    }

    .btn.red:hover {
      background: var(--danger-hover);
      box-shadow: 0 12px 24px -4px rgba(239, 68, 68, 0.45);
    }

    .btn.gray {
      background: var(--card-bg);
      color: var(--text-main);
      border: 1px solid var(--border);
      box-shadow: 0 4px 10px -2px rgba(15, 23, 42, 0.03);
    }

    .btn.gray:hover {
      background: var(--card-hover);
      border-color: var(--text-sub);
      box-shadow: 0 8px 16px -4px rgba(15, 23, 42, 0.06);
    }

    /* Состояние загрузки */
    .btn.loading {
      opacity: 0.75;
      pointer-events: none;
    }

    .btn.loading::after {
      content: '';
      position: absolute;
      right: 20px;
      top: 50%;
      width: 16px;
      height: 16px;
      margin-top: -8px;
      border: 2px solid currentColor;
      border-top-color: transparent;
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }

    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }

    /* Модальные окна */
    .modal-overlay {
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(15, 23, 42, 0.6);
      backdrop-filter: blur(8px);
      -webkit-backdrop-filter: blur(8px);
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
      z-index: 1000;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.3s var(--cubic-smooth);
    }

    .modal-overlay.active {
      opacity: 1;
      pointer-events: auto;
    }

    .modal-card {
      background: var(--card-bg);
      width: 100%;
      max-width: 360px;
      padding: 28px 24px;
      border-radius: 24px;
      border: 1px solid var(--border);
      box-shadow: 0 20px 40px -10px rgba(15, 23, 42, 0.2);
      text-align: center;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 16px;
      transform: scale(0.92) translateY(10px);
      transition: transform 0.3s var(--cubic-smooth);
    }

    .modal-overlay.active .modal-card {
      transform: scale(1) translateY(0);
    }

    .modal-spinner {
      width: 48px;
      height: 48px;
      border: 3px solid var(--border);
      border-top-color: var(--primary);
      border-radius: 50%;
      animation: spin 0.9s linear infinite;
    }

    .modal-title {
      font-size: 1.25rem;
      font-weight: 800;
      color: var(--text-main);
      letter-spacing: -0.01em;
    }

    .modal-desc {
      font-size: 0.95rem;
      color: var(--text-sub);
      line-height: 1.5;
    }

    /* Эффект волны Ripple */
    .ripple {
      position: absolute;
      border-radius: 50%;
      transform: scale(0);
      animation: ripple .6s linear;
      background: rgba(255,255,255,.4);
      pointer-events: none;
    }

    .btn.gray .ripple {
      background: rgba(15, 23, 42, 0.1);
    }

    @keyframes ripple {
      to {
        transform: scale(4);
        opacity: 0;
      }
    }

    /* Анимации появление/уход */
    @keyframes fadeUp {
      from {
        opacity: 0;
        transform: translateY(14px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());
    document.addEventListener('copy', e => e.preventDefault());
    document.addEventListener('cut', e => e.preventDefault());

    function clickSound() {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(550, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    }

    function createRipple(event) {
      clickSound();
      const button = event.currentTarget;
      const circle = document.createElement("span");
      const diameter = Math.max(button.clientWidth, button.clientHeight);

      circle.style.width = circle.style.height = `${diameter}px`;
      circle.style.left = `${event.clientX - button.getBoundingClientRect().left - diameter / 2}px`;
      circle.style.top = `${event.clientY - button.getBoundingClientRect().top - diameter / 2}px`;
      circle.classList.add("ripple");

      const ripple = button.getElementsByClassName("ripple")[0];
      if (ripple) {
        ripple.remove();
      }
      button.appendChild(circle);
    }

    async function checkUpdate(event) {
      createRipple(event);
      const btn = document.getElementById('checkBtn');
      btn.classList.add('loading');

      try {
        await fetch('/CHECKUPDATE');
        const res = await fetch('/STATUS');
        const data = await res.json();

        document.getElementById("remote").innerText = data.remoteVersion || "—";

        let statusText = "";
        if (!data.remoteVersion || data.remoteVersion === "") {
          statusText = "❌ " + tr("Nicht überprüft", "Не перевірено", "Not checked");
        } else if (data.updateAvailable) {
          statusText = "🟡 " + tr("Update verfügbar", "Доступне оновлення", "Update available");
        } else {
          statusText = "🟢 " + tr("Sie haben die neueste Version", "У вас остання версія", "You have the latest version");
        }

        document.getElementById("statusText").innerText = statusText;

        let actionsGrid = document.getElementById("actionsGrid");
        let updateBtn = document.getElementById("updateBtn");

        if (data.updateAvailable && !updateBtn) {
          updateBtn = document.createElement("div");
          updateBtn.className = "btn red";
          updateBtn.id = "updateBtn";
          updateBtn.innerHTML = `
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
            ` + tr("Aktualisieren", "Оновити", "Update") + `
          `;
          updateBtn.onclick = startUpdate;
          actionsGrid.insertBefore(updateBtn, document.getElementById("refreshBtn"));
        } else if (!data.updateAvailable && updateBtn) {
          updateBtn.remove();
        }
      } catch(e) {
        console.log(e);
      }

      setTimeout(() => {
        btn.classList.remove('loading');
      }, 800);
    }

    async function startUpdate(event) {
      createRipple(event);
      
      // Показываем модальное окно загрузки
      document.getElementById('updateModal').classList.add('active');

      try {
        await fetch('/STARTUPDATE');
      } catch(e) {
        console.log(e);
      }
    }

    function refreshPage(event) {
      createRipple(event);
      setTimeout(() => {
        location.reload();
      }, 250);
    }

    function goBack(event) {
      createRipple(event);
      const container = document.querySelector('.container');
      container.classList.add('fadeOut');
      setTimeout(() => {
        history.back();
      }, 300);
    }
  </script>
</head>
<body>

  <div class="container">
    <header>
      <div onclick="goBack(event)" class="back-btn">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M19 12H5"/><path d="M12 19l-7-7 7-7"/></svg>
      </div>
      <div class="header-title">)====="
          + tr("Software-Update", "Оновлення ПЗ", "Software Update") + R"=====(</div>
      <div style="width: 44px;"></div>
    </header>

    <div class="card">
      <div class="version-box">
        <div class="version-row">
          <span>)====="
          + tr("Aktuelle Version:", "Поточна версія:", "Current version:") + R"=====(</span>
          <span class="version-badge" id="fw">v)====="
          + String(FW_VERSION) + R"=====(</span>
        </div>
        <div class="version-row">
          <span>)====="
          + tr("Verfügbare Version:", "Доступна версія:", "Available version:") + R"=====(</span>
          <span class="version-badge" id="remote">)====="
          + (remoteVersion.length() > 0 ? remoteVersion : "—") + R"=====(</span>
        </div>
      </div>

      <div class="status-text" id="statusText">)====="
          + status + R"=====(</div>

      <div class="actions-grid" id="actionsGrid">
        <div class="btn" id="checkBtn" onclick="checkUpdate(event)">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M21.5 2v6h-6"/><path d="M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
          )====="
          + tr("Nach Updates suchen", "Перевірити оновлення", "Check for updates") + R"=====(
        </div>
)=====";

  if (updateAvailableFlag) {
    html += R"=====(
        <div class="btn red" id="updateBtn" onclick="startUpdate(event)">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
          )====="
            + tr("Aktualisieren", "Оновити", "Update") + R"=====(
        </div>
)=====";
  }

  html += R"=====(
        <div class="btn gray" id="refreshBtn" onclick="refreshPage(event)">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M23 4v6h-6"/><path d="M1 20v-6h6"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
          )====="
          + tr("Seite aktualisieren", "Оновить сторінку", "Refresh page") + R"=====(
        </div>
      </div>
    </div>
  </div>

  <!-- Модальное окно обновления -->
  <div class="modal-overlay" id="updateModal">
    <div class="modal-card">
      <div class="modal-spinner"></div>
      <div class="modal-title">)====="
          + tr("Aktualisierung läuft...", "Триває оновлення...", "Updating...") + R"=====(</div>
      <div class="modal-desc">)====="
          + tr("Bitte warten. Das Gerät wird heruntergeladen und neu gestartet.", "Будь ласка, зачекайте. Завантажується нове ПЗ і пристрій перезавантажиться.", "Please wait. Downloading software and restarting device.") + R"=====(</div>
    </div>
  </div>

</body>
</html>)=====";

  return html;
}

String aboutPage() {
  // Заранее определяем все локализованные строки
  String titleText = tr("Über die Firmware", "Про прошивку", "About Firmware");
  String subtitleText = tr("Informationen zum TargetiX-System", "Інформація про систему TargetiX", "TargetiX system information");
  String labelDevice = tr("Gerät", "Пристрій", "Device");
  String labelVersion = tr("Version", "Версія", "Version");
  String labelBuild = tr("Build", "Збірка", "Build");
  String btnBackText = tr("Zurück", "Назад", "Back");

  // Собираем HTML, используя переменные
  String html = R"raw(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
  <meta http-equiv="Pragma" content="no-cache">
  <meta http-equiv="Expires" content="0">
  <title>TargetiX — Über das System</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --border: #e2e8f0;
      --status-online: #10b981;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    body {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      color: var(--text-main);
      padding: 20px;
      overflow-x: hidden;
      animation: ambientBreath 8s ease-in-out infinite alternate;
    }

    @keyframes ambientBreath {
      0% { background-position: 0% 0%, 0 0; }
      100% { background-position: 0% 0%, 12px 12px; }
    }

    .container {
      width: min(360px, 100%);
      display: flex;
      flex-direction: column;
      align-items: center;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
    }

    .header {
      text-align: center;
      margin-bottom: 20px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards;
    }

    .title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }

    .subtitle {
      font-size: 0.875rem;
      color: var(--text-sub);
      font-weight: 500;
      margin-top: 4px;
    }

    .card {
      width: 100%;
      background: var(--card-bg);
      padding: 24px;
      border-radius: 24px;
      border: 1px solid var(--border);
      box-shadow: 0 10px 25px -5px rgba(15, 23, 42, 0.03), 
                  0 8px 10px -6px rgba(15, 23, 42, 0.02);
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }

    .info-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 10px 14px;
      background: var(--bg-color);
      border-radius: 14px;
      border: 1px solid var(--border);
    }

    .info-label {
      font-size: 0.85rem;
      font-weight: 600;
      color: var(--text-sub);
    }

    .info-value {
      font-size: 0.95rem;
      font-weight: 700;
      color: var(--text-main);
    }

    .btn-back {
      width: 100%;
      padding: 14px 0;
      margin-top: 12px;
      border-radius: 14px;
      font-size: 0.9rem;
      font-weight: 600;
      color: var(--text-sub);
      background: var(--card-hover);
      border: 1px solid var(--border);
      cursor: pointer;
      text-align: center;
      transition: all 0.3s var(--cubic-smooth);
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards;
    }

    .btn-back:hover {
      background: var(--card-bg);
      color: var(--text-main);
      border-color: var(--text-sub);
      transform: translateY(-1px);
    }

    .btn-back:active { transform: translateY(1px) scale(0.98); }

    @keyframes fadeUp {
      from { opacity: 0; transform: translateY(14px); }
      to { opacity: 1; transform: translateY(0); }
    }

    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());

    const clickSound = () => {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(550, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    };

    const goBack = () => {
      clickSound();
      const container = document.querySelector('.container');
      container.classList.add('fadeOut');
      setTimeout(() => {
        history.back();
      }, 300);
    };
  </script>
</head>
<body>

  <div class="container">
    <div class="header">
      <div class="title">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/>
        </svg>
        <span>)raw"
                + titleText + R"raw(</span>
      </div>
      <div class="subtitle">)raw"
                + subtitleText + R"raw(</div>
    </div>

    <div class="card">
      <div class="info-row">
        <span class="info-label">)raw"
                + labelDevice + R"raw(</span>
        <span class="info-value">TargetiX</span>
      </div>

      <div class="info-row">
        <span class="info-label">)raw"
                + labelVersion + R"raw(</span>
        <span class="info-value">)raw"
                + String(FW_VERSION) + R"raw(</span>
      </div>

      <div class="info-row">
        <span class="info-label">)raw"
                + labelBuild + R"raw(</span>
        <span class="info-value">)raw"
                + String(__DATE__) + R"raw(</span>
      </div>
    </div>

    <div class="btn-back" onclick="goBack()">
      &#8592; )raw"
                + btnBackText + R"raw(
    </div>
  </div>

</body>
</html>
)raw";

  return html;
}

String cardPage(uint8_t cardId) {
  if (cardId >= cardCount) return tr("Fehler: Karte nicht gefunden", "Помилка: немає такої картки", "Error: Card not found");

  ProgramCard& c = cards[cardId];
  String html;
  html.reserve(7000);

  html += R"raw(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
  <meta http-equiv="Pragma" content="no-cache">
  <meta http-equiv="Expires" content="0">
  <title>)raw";

  html += String(c.title);

  html += R"raw( — TargetiX</title>
  <style>
    :root {
      --bg-color: #f8fafc;
      --card-bg: #ffffff;
      --card-hover: #f1f5f9;
      --text-main: #0f172a;
      --text-sub: #64748b;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --border: #e2e8f0;
      --danger: #ef4444;
      --danger-hover: #dc2626;
      --cubic-smooth: cubic-bezier(0.16, 1, 0.3, 1);
    }

    /* Автоматическая тёмная тема */
    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #0f172a;
        --card-bg: #1e293b;
        --card-hover: #334155;
        --text-main: #f8fafc;
        --text-sub: #94a3b8;
        --border: #334155;
      }
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }

    input, textarea, select {
      user-select: text;
      -webkit-user-select: text;
    }

    body {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background-color: var(--bg-color);
      background-image: 
        radial-gradient(circle at 50% 50%, rgba(37, 99, 235, 0.05) 0%, transparent 60%),
        radial-gradient(var(--border) 1px, transparent 1px);
      background-size: 100% 100%, 24px 24px;
      color: var(--text-main);
      padding: 20px 20px 80px 20px;
      overflow-x: hidden;
      animation: ambientBreath 8s ease-in-out infinite alternate;
    }

    @keyframes ambientBreath {
      0% { background-position: 0% 0%, 0 0; }
      100% { background-position: 0% 0%, 12px 12px; }
    }

    .container {
      width: min(400px, 100%);
      display: flex;
      flex-direction: column;
      align-items: center;
      transition: transform 0.35s var(--cubic-smooth), opacity 0.35s var(--cubic-smooth);
    }

    .header {
      text-align: center;
      margin-bottom: 20px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.1s forwards;
    }

    .title {
      font-size: 1.5rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }

    .subtitle {
      font-size: 0.875rem;
      color: var(--text-sub);
      font-weight: 500;
      margin-top: 4px;
    }

    .grid {
      width: 100%;
      display: flex;
      flex-direction: column;
      gap: 14px;
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.2s forwards;
    }

    .card {
      width: 100%;
      background: var(--card-bg);
      padding: 20px;
      border-radius: 20px;
      border: 1px solid var(--border);
      box-shadow: 0 10px 25px -5px rgba(15, 23, 42, 0.03), 
                  0 8px 10px -6px rgba(15, 23, 42, 0.02);
      cursor: pointer;
      transition: all 0.3s var(--cubic-smooth);
      display: flex;
      flex-direction: column;
      gap: 6px;
    }

    .card:hover {
      transform: translateY(-2px);
      box-shadow: 0 14px 28px -4px rgba(15, 23, 42, 0.08);
      border-color: var(--text-sub);
    }

    .card:active {
      transform: translateY(1px) scale(0.98);
    }

    .card.locked {
      opacity: 0.45;
      background: var(--card-hover) !important;
      pointer-events: none;
      filter: grayscale(40%);
      border-color: var(--border) !important;
      box-shadow: none !important;
      transform: none !important;
    }

    .prog-title {
      font-size: 1.15rem;
      font-weight: 700;
      color: var(--primary);
      word-break: break-word;
    }

    .prog-sub {
      font-size: 0.85rem;
      font-weight: 600;
      color: var(--text-sub);
      display: flex;
      gap: 10px;
      align-items: center;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      padding: 4px 10px;
      border-radius: 8px;
      background: var(--bg-color);
      border: 1px solid var(--border);
      color: var(--text-main);
      font-size: 0.8rem;
    }

    /* Кнопка отмены/назад */
    .btn-back {
      width: 100%;
      padding: 14px 0;
      margin-top: 16px;
      border-radius: 14px;
      font-size: 0.9rem;
      font-weight: 600;
      color: var(--text-sub);
      background: var(--card-hover);
      border: 1px solid var(--border);
      cursor: pointer;
      text-align: center;
      transition: all 0.3s var(--cubic-smooth);
      opacity: 0;
      animation: fadeUp 0.6s var(--cubic-smooth) 0.3s forwards;
    }

    .btn-back:hover {
      background: var(--card-bg);
      color: var(--text-main);
      border-color: var(--text-sub);
      transform: translateY(-1px);
    }

    .btn-back:active { transform: translateY(1px) scale(0.98); }

    /* Красная пульсирующая кнопка STOP */
    .stop-btn {
      position: fixed;
      bottom: 24px;
      left: 50%;
      transform: translateX(-50%);
      width: min(360px, calc(100% - 40px));
      padding: 16px 0;
      border-radius: 16px;
      background: var(--danger);
      color: #ffffff;
      font-size: 1.1rem;
      font-weight: 800;
      letter-spacing: 0.05em;
      text-align: center;
      cursor: pointer;
      display: none;
      box-shadow: 0 10px 25px rgba(239, 68, 68, 0.4);
      z-index: 99;
      animation: stopPulse 1.5s infinite alternate var(--cubic-smooth);
      border: none;
      outline: none;
    }

    @keyframes stopPulse {
      0% { transform: translateX(-50%) scale(1); box-shadow: 0 8px 20px rgba(239, 68, 68, 0.4); }
      100% { transform: translateX(-50%) scale(1.03); box-shadow: 0 14px 30px rgba(239, 68, 68, 0.6); }
    }

    /* Модальное окно */
    .overlay {
      position: fixed;
      inset: 0;
      background: rgba(15, 23, 42, 0.4);
      backdrop-filter: blur(4px);
      -webkit-backdrop-filter: blur(4px);
      display: none;
      align-items: center;
      justify-content: center;
      z-index: 100;
      padding: 20px;
      opacity: 0;
      transition: opacity 0.3s var(--cubic-smooth);
    }

    .overlay.active {
      display: flex;
      opacity: 1;
    }

    .modal {
      background: var(--card-bg);
      border-radius: 24px;
      padding: 24px;
      width: min(320px, 100%);
      text-align: center;
      box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.1), 0 8px 10px -6px rgba(0, 0, 0, 0.1);
      border: 1px solid var(--border);
      transform: scale(0.9);
      transition: transform 0.3s var(--cubic-smooth);
    }

    .overlay.active .modal {
      transform: scale(1);
    }

    .modal h2 {
      font-size: 1.2rem;
      font-weight: 700;
      color: var(--text-main);
      margin-bottom: 8px;
    }

    .modal p {
      font-size: 0.875rem;
      color: var(--text-sub);
      margin-bottom: 20px;
    }

    .modal-actions {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    .btn-del {
      width: 100%;
      padding: 12px 0;
      border-radius: 12px;
      font-size: 0.95rem;
      font-weight: 600;
      border: none;
      cursor: pointer;
      transition: all 0.2s var(--cubic-smooth);
      background: var(--danger);
      color: white;
    }

    .btn-del:hover { background: var(--danger-hover); }

    .btn-cancel {
      width: 100%;
      padding: 12px 0;
      border-radius: 12px;
      font-size: 0.95rem;
      font-weight: 600;
      border: 1px solid var(--border);
      cursor: pointer;
      transition: all 0.2s var(--cubic-smooth);
      background: var(--card-hover);
      color: var(--text-sub);
    }

    .btn-cancel:hover { background: var(--card-bg); color: var(--text-main); border-color: var(--text-sub); }

    @keyframes fadeUp {
      from { opacity: 0; transform: translateY(14px); }
      to { opacity: 1; transform: translateY(0); }
    }

    .fadeOut {
      opacity: 0 !important;
      transform: translateY(-10px) scale(0.96) !important;
    }
  </style>

  <script>
    let programRunning = false;
    let soundEnabled = localStorage.getItem('sound') !== 'off';
    let pressTimer = null;
    let delC = 0, delP = 0;

    document.addEventListener('contextmenu', e => e.preventDefault());

    const clickSound = () => {
      try {
        const ctx = new (window.AudioContext || window.webkitAudioContext)();
        const o = ctx.createOscillator();
        const g = ctx.createGain();
        o.type = 'sine';
        o.frequency.setValueAtTime(550, ctx.currentTime);
        o.frequency.exponentialRampToValueAtTime(320, ctx.currentTime + 0.08);
        g.gain.setValueAtTime(0.06, ctx.currentTime);
        g.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.08);
        o.connect(g);
        g.connect(ctx.destination);
        o.start();
        setTimeout(() => { o.stop(); ctx.close(); }, 90);
      } catch(e) {}

      try {
        if (navigator.vibrate) navigator.vibrate(40);
      } catch(e) {}
    };

    const speak = (t) => {
      if (!soundEnabled) return;
      let u = new SpeechSynthesisUtterance(t);
      u.lang = 'en-US';
      speechSynthesis.speak(u);
    };

    const lockCards = (s) => {
      document.querySelectorAll('.card').forEach(c => s ? c.classList.add('locked') : c.classList.remove('locked'));
    };

    const runCard = async (url) => {
      if (programRunning) return;
      clickSound();
      programRunning = true;
      lockCards(true);
      if (soundEnabled) {
        speak('Load');
        await new Promise(r => setTimeout(r, 3000));
      }
      fetch(url);
      if (soundEnabled) speak('Attention');
    };

    const stopOnly = () => {
      clickSound();
      fetch('/STOP');
    };

    const lpStart = (c, p) => {
      pressTimer = setTimeout(() => {
        delC = c;
        delP = p;
        const ov = document.getElementById('overlay');
        ov.classList.add('active');
      }, 700);
    };

    const lpEnd = () => {
      clearTimeout(pressTimer);
    };

    const closeOverlay = () => {
      clickSound();
      const ov = document.getElementById('overlay');
      ov.classList.remove('active');
    };

    const confirmDel = () => {
      clickSound();
      fetch('/DELPROG?c=' + delC + '&p=' + delP).then(() => location.reload());
    };

    const upd = async () => {
      try {
        let r = await fetch('/STATUS');
        let t = (await r.text()).trim();
        if (t.startsWith('RUNNING')) {
          programRunning = true;
          lockCards(true);
          document.getElementById('stopBtn').style.display = 'block';
        } else {
          programRunning = false;
          lockCards(false);
          document.getElementById('stopBtn').style.display = 'none';
        }
      } catch(e) {}
    };

    const goBack = () => {
      clickSound();
      const container = document.querySelector('.container');
      container.classList.add('fadeOut');
      setTimeout(() => {
        history.back();
      }, 300);
    };

    setInterval(upd, 500);
    upd();
  </script>
</head>
<body>

  <div class="container">
    <div class="header">
      <div class="title">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="9" y1="3" x2="9" y2="21"/>
        </svg>
        <span>)raw";

  html += String(c.title);

  html += R"raw(</span>
      </div>
      <div class="subtitle">)raw"
          + tr("Liste der Kartenprogramme", "Список програм картки", "Card programs list") + R"raw(</div>
    </div>

    <div class="grid">)raw";

  /* ===== СНИППЕТ ПРОГРАММ ===== */
  for (uint8_t p = 0; p < c.count; p++) {
    UserProgram& pr = c.programs[p];
    html += "<div class='card' ";
    html += "onmousedown='lpStart(" + String(cardId) + "," + String(p) + ")' ";
    html += "onmouseup='lpEnd()' ";
    html += "ontouchstart='lpStart(" + String(cardId) + "," + String(p) + ")' ";
    html += "ontouchend='lpEnd()' ";
    html += "onclick=\"runCard('/RUN?c=" + String(cardId) + "&p=" + String(p) + "')\">";
    html += "<div class='prog-title'>" + String(pr.name) + "</div>";
    html += "<div class='prog-sub'>";
    html += "<span class='badge'>R: " + String(pr.redSec) + "s</span>";
    html += "<span class='badge'>G: " + String(pr.greenSec) + "s</span>";
    html += "<span class='badge'>×" + String(pr.series) + "</span>";
    html += "</div>";
    html += "</div>";
  }

  html += R"raw(
    </div>

    <div class="btn-back" onclick="goBack()">
      &#8592; )raw"
          + tr("Zurück", "Назад", "Back") + R"raw(
    </div>
  </div>

  <!-- Модальное окно подтверждения удаления -->
  <div id="overlay" class="overlay">
    <div class="modal">
      <h2>)raw"
          + tr("Programm löschen?", "Видалити програму?", "Delete program?") + R"raw(</h2>
      <p>)raw"
          + tr("Möchten Sie dieses Programm wirklich unwiderruflich löschen?", "Ви дійсно хочете безповоротно видалити цю програму?", "Do you really want to permanently delete this program?") + R"raw(</p>
      <div class="modal-actions">
        <button class="btn-del" onclick="confirmDel()">)raw"
          + tr("Löschen", "Видалити", "Delete") + R"raw(</button>
        <button class="btn-cancel" onclick="closeOverlay()">)raw"
          + tr("Abbrechen", "Скасувати", "Cancel") + R"raw(</button>
      </div>
    </div>
  </div>

  <!-- Экстренная кнопка остановки -->
  <button id="stopBtn" class="stop-btn" onclick="stopOnly()">⛔ )raw"
          + tr("STOPP", "СТОП", "STOP") + R"raw(</button>

</body>
</html>
)raw";

  return html;
}

void saveWiFiCredentials(String ssid, String pass) {

  EEPROM.begin(EEPROM_SIZE);

  uint8_t flag = 1;

  EEPROM.put(EEPROM_WIFI_FLAG, flag);

  char ssidBuf[32];
  char passBuf[64];

  memset(ssidBuf, 0, sizeof(ssidBuf));
  memset(passBuf, 0, sizeof(passBuf));

  strncpy(ssidBuf, ssid.c_str(), sizeof(ssidBuf) - 1);
  strncpy(passBuf, pass.c_str(), sizeof(passBuf) - 1);

  EEPROM.put(EEPROM_WIFI_SSID, ssidBuf);
  EEPROM.put(EEPROM_WIFI_PASS, passBuf);

  EEPROM.commit();
  EEPROM.end();

  Serial.println("💾 Wi-Fi сохранён");
}


void connectToWiFi(String ssid, String pass) {

  Serial.println("📶 Подключение к Wi‑Fi...");

  enableSTA(ssid, pass);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi‑Fi подключен");
    Serial.println(WiFi.localIP());

    savedSSID = ssid;
    savedPASS = pass;

    wifiSetupCompleted = true;
    saveWiFiCredentials(ssid, pass);
  } else {
    Serial.println("\n❌ Ошибка подключения");
  }
}





void handleRequest(WiFiClient& client, String request) {

  // Обработка перезагрузки устройства (поставить в само НАЧАЛО handleRequest)
  if (request.indexOf("GET /REBOOT") >= 0) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println("Access-Control-Allow-Origin: *");
    client.println();
    client.println("Rebooting...");
    client.stop();  // Принудительно закрываем соединение с клиентом

    delay(1000);    // Ждем 1 секунду, чтобы браузер успел показать модальное окно "Подождите"
    ESP.restart();  // Перезапуск ESP32
    return;
  }

// Проверяем, содержит ли запрос путь /api/ready
if (request.indexOf("/api/ready") >= 0) {
  bool isReady = true; // Для теста ставим true

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println("Access-Control-Allow-Origin: *"); // На всякий случай разрешаем CORS
  client.println();
  
  if (isReady) {
    client.println("{\"ready\":true}");
  } else {
    client.println("{\"ready\":false}");
  }
  
  client.flush();
  client.stop(); // Обязательно закрываем соединение с клиентом для fetch
  return;
}

  /* ===== STATUS ===== */
  if (request.startsWith("GET /STATUS")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    if (isProgramRunning)
      client.println("RUNNING:" + String(activeProgramId));
    else
      client.println("STOPPED");
    return;
  }

  /* ===== MP8 PAGE ===== */
  if (request.startsWith("GET /MP8")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(mp8Page());
    return;
  }

  /* ===== MP10 PAGE ===== */
  if (request.startsWith("GET /MP10")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(mp10Page());
    return;
  }
  /* ===== ADD PROGRAM PAGE ===== */
  if (request.startsWith("GET /ADD")) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(addPage());
    return;
  }


  /* ===== RUN USER PROGRAM ===== */
  if (request.startsWith("GET /RUN?")) {
    int c = getParam(request, "c").toInt();
    int p = getParam(request, "p").toInt();

    if (c < cardCount && p < cards[c].count) {
      UserProgram& pr = cards[c].programs[p];
      startProgram(pr.redSec, pr.greenSec, pr.series);
    }

    client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
    return;
  }


  /* ===== RUN PRESET ===== */
  if (request.startsWith("GET /RUNPRESET")) {
    String mode = getParam(request, "mode");
    int g = getParam(request, "g").toInt();

    if (mode == "MP5") {
      activeProgramId = -10;
      startProgram(7, 3, 5);
    } else if (mode == "MP8") {
      activeProgramId = -11;
      startProgram(7, g, 1);
    } else if (mode == "MP10") {
      activeProgramId = -12;
      startProgram(7, g, 1);
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Connection: close");
    client.println();
    return;
  }

  /* ===== STOP ===== */
  if (request.startsWith("GET /STOP")) {
    stopProgram();
    activeProgramId = -1;
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }

  /* ===== CLEAR ALL ===== */
  if (request.startsWith("GET /CLEAR")) {
    userProgramCount = 0;
    eepromSave();
    activeProgramId = -1;
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }
  /* ===== SAVE PROGRAM ===== */
  if (request.startsWith("GET /SAVE")) {

    String progName = getParam(request, "progname");
    String cardStr = getParam(request, "card");
    String newCard = getParam(request, "newcard");

    uint16_t red = getParam(request, "red").toInt();
    uint16_t green = getParam(request, "green").toInt();
    uint16_t series = getParam(request, "series").toInt();

    if (progName == "" || red == 0 || green == 0 || series == 0) {
      client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
      return;
    }

    uint8_t cardId;

    // ➕ новая карточка
    if (cardStr == "") {
      if (cardCount >= MAX_CARDS || newCard == "") {
        client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
        return;
      }

      cardId = cardCount++;
      memset(&cards[cardId], 0, sizeof(ProgramCard));
      strncpy(cards[cardId].title, newCard.c_str(), 19);
    } else {
      cardId = cardStr.toInt();
      if (cardId >= cardCount) return;
    }

    ProgramCard& c = cards[cardId];
    if (c.count >= MAX_PROGRAMS_PER_CARD) return;

    UserProgram& p = c.programs[c.count++];
    memset(&p, 0, sizeof(UserProgram));
    strncpy(p.name, progName.c_str(), 19);
    p.redSec = red;
    p.greenSec = green;
    p.series = series;

    eepromSave();

    client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
    return;
  }

  // ===== CARD PAGES =====
  if (request.startsWith("GET /CARD?")) {
    int c = getParam(request, "id").toInt();
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print(cardPage(c));
    return;
  }

  // ===== Удалить программу =====
  if (request.startsWith("GET /DELPROG")) {
    int c = getParam(request, "c").toInt();
    int p = getParam(request, "p").toInt();
    if (c < cardCount && p < cards[c].count) {
      // Сдвигаем все программы после p на одну позицию влево
      for (int i = p; i < cards[c].count - 1; i++) {
        cards[c].programs[i] = cards[c].programs[i + 1];
      }
      cards[c].count--;
      eepromSave();
    }
    client.println("HTTP/1.1 303 See Other\r\nLocation: /CARD?id=" + String(c) + "\r\n");
    return;
  }

  // ===== Удалить карточку =====
  if (request.startsWith("GET /DELCARD")) {
    int id = getParam(request, "id").toInt();
    if (id < cardCount) {
      // Сдвигаем все карточки после id на одну позицию влево
      for (int i = id; i < cardCount - 1; i++) {
        cards[i] = cards[i + 1];
      }
      cardCount--;
      eepromSave();
    }
    client.println("HTTP/1.1 303 See Other\r\nLocation: /\r\n");
    return;
  }


  /* ===== RUN LAST PROGRAM ===== */
  if (request.startsWith("GET /RUNLAST")) {

    if (lastRed > 0 && lastGreen > 0 && lastSeries > 0) {
      activeProgramId = -99;  // ID для last program
      startProgram(lastRed, lastGreen, lastSeries);
      Serial.println("▶️ Запуск последней программы (RUNLAST)");
    } else {
      Serial.println("⚠️ Нет сохранённой последней программы");
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Connection: close");
    client.println();
    return;
  }

  // ===== TOGGLE START / STOP =====
  if (request.startsWith("GET /TOGGLE")) {

    if (isProgramRunning) {
      stopProgram();
    } else {
      if (lastRed && lastGreen && lastSeries) {
        startProgram(lastRed, lastGreen, lastSeries);
      }
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Connection: close");
    client.println();
    return;
  }


  /* ===== SET LANGUAGE ===== */
  if (request.startsWith("GET /SETLANG")) {

    String l = getParam(request, "l");

    if (l == "de") currentLang = LANG_DE;
    else if (l == "ua") currentLang = LANG_UA;
    else if (l == "en") currentLang = LANG_EN;

    setupCompleted = true;
    saveLanguage();

    client.println("HTTP/1.1 303 See Other");
    wifiSetupCompleted = true;

    client.println("Location: /");
    client.println();
    return;
  }

  if (request.startsWith("GET /STARTSETUP")) {

    welcomeCompleted = true;
    saveWelcome();

    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }

  /* ===== WIFI PAGE ===== */
  if (request.startsWith("GET /WIFISETUP")) {

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();

    client.print(wifiSetupPage());
    return;
  }

  /* ===== CONNECT WIFI ===== */
  if (request.startsWith("GET /CONNECTWIFI")) {

    String ssid = getParam(request, "ssid");
    String pass = getParam(request, "pass");

    connectToWiFi(ssid, pass);

    // ❗ НЕ редиректим в главное меню
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();

    client.print(wifiSetupPage());
    return;
  }

  if (request.startsWith("GET /STARTUPDATE")) {

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("Updating...");

    performOTAUpdate();

    return;
  }

  if (request.startsWith("GET /UPDATE")) {

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();

    client.print(updatePage());
    return;
  }

  if (request.startsWith("GET /CHECKUPDATE")) {

    checkVersion();

    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /UPDATE");
    client.println();
    return;
  }

  /* ===== WIFI STA OFF ===== */
  if (request.startsWith("GET /WIFIOFF")) {

    disableSTA();

    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }

  /* ===== WIFI STA ON ===== */
  if (request.startsWith("GET /WIFION")) {

    if (savedSSID.length() > 0) {

      enableSTA(savedSSID, savedPASS);
    }

    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();
    return;
  }

  /* ===== OTA PROGRESS ===== */
  if (request.startsWith("GET /UPDATEPROGRESS")) {

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/event-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println();

    int last = -1;

    while (client.connected()) {

      if (otaProgress != last) {

        client.print("data: ");
        client.print(otaProgress);
        client.print("\n\n");

        last = otaProgress;
      }

      delay(200);
    }

    return;
  }

  /* ===== SETTINGS PAGE ===== */
  if (request.startsWith("GET /SETTINGS")) {

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();

    client.print(settingsPage());

    return;
  }

  if (request.startsWith("GET /RESET")) {
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /");
    client.println();

    factoryReset();
    return;
  }

  /* ===== ABOUT FIRMWARE ===== */
  if (request.startsWith("GET /ABOUT")) {

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();

    client.print(aboutPage());
    return;
  }

  /* ===== UNKNOWN URL REDIRECT ===== */

  if (
    request.indexOf("GET / ") == -1 && request.indexOf("GET /STATUS") == -1 && request.indexOf("GET /MP8") == -1 && request.indexOf("GET /MP10") == -1) {

    client.println("HTTP/1.1 302 Found");
    client.println("Location: http://192.168.2.5/");
    client.println("Connection: close");
    client.println();

    return;
  }

  /* ===== MAIN PAGE (ВСЕГДА В КОНЦЕ) ===== */

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  if (!welcomeCompleted) {

    client.print(welcomePage());

  } else if (!setupCompleted) {

    client.print(setupPage());

  } else {

    client.print(mainPage());
  }

  return;
}



// ====== Setup / Loop ======
void setup() {
  Serial.begin(115200);
  pinMode(redLedPin, OUTPUT);
  pinMode(greenLedPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);

  setupWiFi();
  loadWelcome();
  loadLanguage();
  eepromLoad();
  loadLastProgram();  // загружаем последнюю программу


  String s, p;

  if (loadWiFiCredentials(s, p)) {

    savedSSID = s;
    savedPASS = p;

    Serial.println("📶 Автоподключение...");

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(s.c_str(), p.c_str());

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {

      delay(300);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {

      wifiSetupCompleted = true;

      Serial.println("");
      Serial.println("✅ Wi-Fi подключен");
      Serial.println(WiFi.localIP());
    }
  }

  ArduinoOTA.setHostname("ESP32_Controller");
  ArduinoOTA.onStart([]() {
    Serial.println("🔄 OTA старт...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("✅ OTA ок");
    lastOTAUpdate = String(__DATE__) + " " + String(__TIME__);  // время последнего обновления
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("❌ OTA ошибка [%u]\n", error);
  });
  ArduinoOTA.begin();
}

void loop() {
  handleButton();
  handleProgramLogic();
  ArduinoOTA.handle();

  dnsServer.processNextRequest();

  WiFiClient client = server.available();
  if (!client) return;

  String request = client.readStringUntil('\r');
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }

  Serial.println("📨 " + request);
  handleRequest(client, request);
  client.stop();
}
