#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>

const char* ssid = "TargetiX_SL7";
const char* password = "sport_pistol";

const char* AP_ssid = "OTA_UPDATE";
const char* AP_password = "button_issf";

const char* serverIP = "192.168.2.7";
const int serverPort = 80;

const int buttonPin = 0;    // D3 (GPIO0)
const int buzzerPin = 14;   // D5 (GPIO14)

WiFiClient client;

bool buttonPressed = false;

// =====================================================
// ГРОМКИЙ ПИСК ДЛЯ ПАССИВНОГО BUZZER
// =====================================================
void beep(int durationMs = 120) {

  // 4 кГц обычно громче всего
  tone(buzzerPin, 4000);

  delay(durationMs);

  noTone(buzzerPin);
}

// =====================================================
// ПРОВЕРКА СТАТУСА
// =====================================================
bool isProgramRunning() {

  WiFiClient statusClient;

  if (!statusClient.connect(serverIP, serverPort)) {

    Serial.println("❌ STATUS: нет соединения");

    return false;
  }

  statusClient.print(
    String("GET /STATUS HTTP/1.1\r\n") +
    "Host: " + serverIP + "\r\n" +
    "Connection: close\r\n\r\n"
  );

  unsigned long timeout = millis();

  while (statusClient.connected() && !statusClient.available()) {

    if (millis() - timeout > 1000) {

      statusClient.stop();

      Serial.println("⏱ STATUS timeout");

      return false;
    }
  }

  String response;

  while (statusClient.available()) {
    response += statusClient.readString();
  }

  statusClient.stop();

  return response.indexOf("RUNNING") != -1;
}

// =====================================================
// ОТПРАВКА HTTP КОМАНДЫ
// =====================================================
void sendRequest(const char* path) {

  if (client.connect(serverIP, serverPort)) {

    client.print(String("GET ") + path + " HTTP/1.1\r\n");
    client.print("Host: " + String(serverIP) + "\r\n");
    client.print("Connection: close\r\n\r\n");

    Serial.println(String("📨 Отправлено: ") + path);

    // короткий громкий сигнал
    beep(60);

    delay(50);

    client.stop();

  } else {

    Serial.println("❌ Ошибка подключения к ESP32");

    // длинный сигнал ошибки
    beep(500);
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {

  Serial.begin(115200);

  pinMode(buttonPin, INPUT_PULLUP);

  pinMode(buzzerPin, OUTPUT);

  digitalWrite(buzzerPin, LOW);

  Serial.println();
  Serial.print("Подключение к ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  // =====================================================
  // ОЖИДАНИЕ WIFI
  // =====================================================
  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");

    // тихий тик во время ожидания
    tone(buzzerPin, 2500);
    delay(25);
    noTone(buzzerPin);
  }

  Serial.println();
  Serial.println("✅ WiFi подключен");

  Serial.print("IP ESP8266: ");
  Serial.println(WiFi.localIP());

  // =====================================================
  // ДВОЙНОЙ СИГНАЛ ПОДКЛЮЧЕНИЯ
  // =====================================================
  beep(150);

  delay(120);

  beep(150);

  // =====================================================
  // OTA
  // =====================================================
  ArduinoOTA.setHostname("esp8266-button");

  ArduinoOTA.onStart([]() {

    Serial.println("🚀 OTA старт");

    beep(250);
  });

  ArduinoOTA.onEnd([]() {

    Serial.println("✅ OTA завершена");

    beep(100);

    delay(100);

    beep(100);

    delay(100);

    beep(100);
  });

  ArduinoOTA.onError([](ota_error_t error) {

    Serial.printf("❌ OTA ошибка: %u\n", error);

    beep(700);
  });

  ArduinoOTA.begin();

  // =====================================================
  // AP ДЛЯ OTA
  // =====================================================
  WiFi.softAP(AP_ssid, AP_password);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

// =====================================================
// LOOP
// =====================================================
void loop() {

  ArduinoOTA.handle();

  bool state = digitalRead(buttonPin);

  // =====================================================
  // НАЖАТИЕ КНОПКИ
  // =====================================================
  if (state == LOW && !buttonPressed) {

    delay(40);

    if (digitalRead(buttonPin) == LOW) {

      buttonPressed = true;

      Serial.println("🔄 TOGGLE");

      sendRequest("/TOGGLE");
    }
  }

  // =====================================================
  // ОТПУСКАНИЕ КНОПКИ
  // =====================================================
  if (state == HIGH && buttonPressed) {

    buttonPressed = false;
  }
}