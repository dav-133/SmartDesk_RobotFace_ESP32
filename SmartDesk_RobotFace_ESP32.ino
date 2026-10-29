/*
  SMART DESK + EXPRESSIVE ROBOT FACE
  Based on the user's existing SH1106 128x64 OLED project.

  EXISTING WIRING (kept unchanged):
    OLED SDA=21, SCL=22
    Touch OUT=32
    DOWN button=18 to GND
    SELECT button=19 to GND
    LED1=23 through 220-330 ohm resistor
    LED2=16 through 220-330 ohm resistor
    Buzzer=25 (use a transistor driver if the buzzer current is too high)

  Libraries:
    U8g2 by olikraus
    ArduinoJson by Benoit Blanchon
  ESP32 Arduino core provides WiFi, WebServer, HTTPClient, Preferences.

  Controls:
    HOME: DOWN cycles emotions; SELECT opens menu; touch changes emotion/wakes.
    MENU: DOWN moves selection; SELECT opens item; long SELECT returns home.
    FACE STUDIO: DOWN changes emotion; SELECT toggles auto blink.
    ALARM: DOWN changes hour; SELECT changes minutes; touch toggles alarm on/off.
    SOUND LAB: DOWN selects sound; SELECT plays it.
    LED LAB: DOWN selects pattern; SELECT toggles LED lab.
    SLEEP: touch or either button wakes the robot.
    Active alarm: touch or SELECT dismisses it.

  Set WIFI_SSID and WIFI_PASSWORD before uploading.
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ---------- OLED: keep the SH1106 driver from the original code ----------
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

// ---------- Existing wiring: do not change your connections ----------
constexpr int TOUCH_PIN = 32;
constexpr int BTN_DOWN = 18;
constexpr int BTN_SELECT = 19;
constexpr int LED1 = 23;
constexpr int LED2 = 16;
constexpr int BUZZER = 25;

// ---------- Wi-Fi: replace these placeholders ----------
const char* WIFI_SSID = "Q-DEVELOPER";
const char* WIFI_PASSWORD = "Fauna@2026";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.google.com";
const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast?latitude=-1.2864&longitude=36.8172"
  "&current=temperature_2m,relative_humidity_2m,weather_code"
  "&timezone=Africa%2FNairobi";

WebServer server(80);
Preferences prefs;

// ---------- Button debouncing and long press ----------
struct Button {
  int pin;
  bool stable;
  bool lastRaw;
  unsigned long changedAt;
  unsigned long pressedAt;
  bool longHandled;
  bool pressEvent;
  bool longEvent;
};
Button downBtn = {BTN_DOWN, HIGH, HIGH, 0, 0, false, false, false};
Button selectBtn = {BTN_SELECT, HIGH, HIGH, 0, 0, false, false, false};

void updateButton(Button &b) {
  const bool raw = digitalRead(b.pin);
  const unsigned long now = millis();
  b.pressEvent = false;
  b.longEvent = false;
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedAt = now;
  }
  if ((now - b.changedAt) >= 30 && raw != b.stable) {
    b.stable = raw;
    if (b.stable == LOW) {
      b.pressedAt = now;
      b.longHandled = false;
      b.pressEvent = true;
    }
  }
  if (b.stable == LOW && !b.longHandled && now - b.pressedAt >= 800) {
    b.longHandled = true;
    b.longEvent = true;
  }
}

// ---------- Screens and emotions ----------
enum Screen : uint8_t {
  HOME, MENU, FACE_STUDIO, SOUND_LAB, LED_LAB, GAMES,
  STATUS_SCREEN, SETTINGS, ALARM_SCREEN, WEATHER_SCREEN, NOTIFY_SCREEN
};
enum Mood : uint8_t {
  NEUTRAL, HAPPY, SAD, ANGRY, CURIOUS, SURPRISED,
  SLEEPY, LOVE, CRYING, CONFUSED, EXCITED, MOOD_COUNT
};

Screen screen = HOME;
Mood mood = HAPPY;
const char* moodNames[MOOD_COUNT] = {
  "NEUTRAL", "HAPPY", "SAD", "ANGRY", "CURIOUS", "SURPRISED",
  "SLEEPY", "LOVE", "CRYING", "CONFUSED", "EXCITED"
};
const char* menuItems[] = {
  "Robot Face", "Face Studio", "Alarm Clock", "Weather",
  "Notifications", "Sound Lab", "LED Lab", "Reaction Game",
  "System Status", "Settings"
};
constexpr int MENU_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);
int menuIndex = 0;
int soundIndex = 0;
int ledMode = 0;
int gameState = 0;
unsigned long gameStarted = 0;
unsigned long gameWait = 0;
unsigned long gameResult = 0;

bool sleeping = false;
bool autoBlink = true;
bool blinking = false;
bool touchWasActive = false;
bool ledPhase = false;
bool ledLabEnabled = false;
int eyeLookX = 0;
unsigned long lastActivity = 0;
unsigned long lastBlink = 0;
unsigned long blinkStarted = 0;
unsigned long lastLook = 0;
unsigned long lastFrame = 0;
unsigned long lastLedTick = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastAutoPage = 0;
unsigned long lastAlarmToggle = 0;
unsigned long lastTouchDebounce = 0;
bool ntpConfigured = false;
bool webServerStarted = false;
bool clockEverSynced = false;

// ---------- Alarm / weather / notifications ----------
int alarmHour = 6;
int alarmMinute = 0;
bool alarmEnabled = true;
bool alarmRinging = false;
bool alarmOutputOn = false;
String lastAlarmDate = "";
String notificationMessage = "No new messages";
String weatherSummary = "Waiting for Wi-Fi";
float weatherTemp = 0.0f;
int weatherHumidity = 0;
int weatherCode = -1;
bool weatherValid = false;

// ---------- Small utilities ----------
void beep(int frequency, int duration) {
  // Non-blocking timed tone on ESP32 Arduino core.
  tone(BUZZER, frequency, duration);
}

void noteActivity() {
  lastActivity = millis();
  if (sleeping) {
    sleeping = false;
    mood = HAPPY;
    screen = HOME;
    blinking = false;
    beep(1350, 70);
  }
}

bool readLocalTime(struct tm &ti) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &ti);
  clockEverSynced = true;
  return true;
}

void maintainWiFiTime() {
  const unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpConfigured) {
      configTzTime("EAT-3", NTP1, NTP2); // Nairobi, UTC+3
      ntpConfigured = true;
      Serial.println("Wi-Fi connected; synchronising Nairobi time.");
      Serial.print("Device IP: http://"); Serial.println(WiFi.localIP());
    }
    return;
  }
  if (now - lastWiFiAttempt >= 30000UL) {
    lastWiFiAttempt = now;
    if (String(WIFI_SSID) == "YOUR_WIFI_NAME") return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Attempting Wi-Fi connection...");
  }
}

void saveAlarm() {
  prefs.putInt("alarmHour", alarmHour);
  prefs.putInt("alarmMin", alarmMinute);
  prefs.putBool("alarmOn", alarmEnabled);
}
void loadAlarm() {
  alarmHour = prefs.getInt("alarmHour", 6);
  alarmMinute = prefs.getInt("alarmMin", 0);
  alarmEnabled = prefs.getBool("alarmOn", true);
  lastAlarmDate = prefs.getString("lastDate", "");
}
void stopAlarm() {
  alarmRinging = false;
  alarmOutputOn = false;
  noTone(BUZZER);
  digitalWrite(LED1, LOW);
  digitalWrite(LED2, LOW);
  lastActivity = millis();
}
void startAlarm() {
  alarmRinging = true;
  alarmOutputOn = false;
  lastAlarmToggle = 0;
  screen = HOME;
  sleeping = false;
  beep(1900, 180);
}

// ---------- Weather: Open-Meteo, no API key required ----------
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weatherSummary = "Wi-Fi offline";
    return;
  }
  WiFiClientSecure client;
  // Convenience setting for a hobby prototype only. For production, verify TLS certificates.
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(client, WEATHER_URL)) {
    weatherSummary = "Weather connection failed";
    return;
  }
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && !doc["current"].isNull()) {
      JsonObject cur = doc["current"];
      weatherTemp = cur["temperature_2m"] | 0.0f;
      weatherHumidity = cur["relative_humidity_2m"] | 0;
      weatherCode = cur["weather_code"] | -1;
      weatherValid = true;
      weatherSummary = "Updated from Open-Meteo";
    } else {
      weatherSummary = "Weather data error";
    }
  } else {
    weatherSummary = "Weather request failed";
  }
  http.end();
  lastWeatherFetch = millis();
}

const char* weatherDescription(int c) {
  if (c == 0) return "Clear";
  if (c == 1 || c == 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Foggy";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Rain showers";
  if (c >= 95) return "Thunderstorm";
  return "Conditions unknown";
}

void checkAlarm() {
  if (!alarmEnabled || alarmRinging) return;
  struct tm ti;
  if (!readLocalTime(ti)) return;
  char dateBuf[11];
  strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &ti);
  String today(dateBuf);
  if (ti.tm_hour == alarmHour && ti.tm_min == alarmMinute && lastAlarmDate != today) {
    lastAlarmDate = today;
    prefs.putString("lastDate", today);
    startAlarm();
  }
}

void updateAlarmOutputs() {
  if (!alarmRinging) return;
  if (millis() - lastAlarmToggle >= 300) {
    lastAlarmToggle = millis();
    alarmOutputOn = !alarmOutputOn;
    digitalWrite(LED1, alarmOutputOn ? HIGH : LOW);
    digitalWrite(LED2, alarmOutputOn ? LOW : HIGH);
    if (alarmOutputOn) tone(BUZZER, 1800);
    else noTone(BUZZER);
  }
}

// ---------- Header and expressive cube eyes ----------
void drawHeader(const char* title) {
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(1, 8, title);
  oled.drawHLine(0, 11, 128);
}
void drawClockHeader() {
  struct tm ti;
  char dateText[12] = "--/--/--";
  char timeText[12] = "--:--:--";
  if (readLocalTime(ti)) {
    strftime(dateText, sizeof(dateText), "%d/%m/%y", &ti);
    strftime(timeText, sizeof(timeText), "%H:%M:%S", &ti);
  }
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(1, 7, dateText);
  oled.drawStr(88, 7, timeText);
  oled.drawHLine(0, 10, 128);
}

// Square/block eyes. White eye blocks, dark pupils, highlights, lids and brows
// are drawn as pixel shapes for the monochrome 128x64 OLED.
void drawCubeEye(int x, int y, int w, int h, int pupilOffset, bool closed, int lid) {
  if (closed) {
    oled.drawBox(x + 2, y + h / 2, w - 4, 3);
    oled.drawPixel(x + 4, y + h / 2 - 2);
    oled.drawPixel(x + w - 5, y + h / 2 - 2);
    return;
  }
  oled.drawFrame(x, y, w, h);
  oled.drawBox(x + 2, y + 2, w - 4, h - 4);
  oled.setDrawColor(0);
  int pupilW = 12;
  int pupilH = 16;
  int px = x + (w - pupilW) / 2 + pupilOffset;
  if (mood == SURPRISED) { pupilW = 8; pupilH = 10; px = x + (w - pupilW) / 2 + pupilOffset; }
  if (mood == SLEEPY) pupilH = 8;
  if (mood == ANGRY) pupilH = 13;
  if (mood == LOVE) { // blocky heart pupils
    int cx = x + w / 2 + pupilOffset;
    int cy = y + h / 2;
    oled.drawBox(cx - 6, cy - 4, 5, 5);
    oled.drawBox(cx + 1, cy - 4, 5, 5);
    oled.drawBox(cx - 7, cy - 1, 14, 5);
    oled.drawTriangle(cx - 7, cy + 2, cx + 7, cy + 2, cx, cy + 10);
  } else {
    oled.drawBox(px, y + (h - pupilH) / 2 + (mood == SLEEPY ? 3 : 0), pupilW, pupilH);
    // Small pixel highlight cut into pupil.
    oled.drawPixel(px + 2, y + h / 2 - 3);
  }
  oled.setDrawColor(1);
  // Eyelid overlay gives sleepiness, anger and sadness a distinct silhouette.
  if (lid > 0) oled.drawBox(x + 1, y + 1, w - 2, lid);
}

void drawTear(int x, int y, int phase) {
  int yy = y + phase;
  oled.drawDisc(x, yy, 2);
  oled.drawTriangle(x - 2, yy, x + 2, yy, x, yy + 5);
}

void drawMouth() {
  switch (mood) {
    case HAPPY:
    case EXCITED:
      oled.drawLine(51, 49, 56, 54); oled.drawLine(56, 54, 72, 54); oled.drawLine(72, 54, 77, 49);
      if (mood == EXCITED) { oled.drawLine(55, 56, 73, 56); oled.drawPixel(49, 45); oled.drawPixel(79, 45); }
      break;
    case SAD:
    case CRYING:
      oled.drawLine(53, 55, 59, 50); oled.drawLine(59, 50, 69, 50); oled.drawLine(69, 50, 75, 55);
      break;
    case ANGRY:
      oled.drawLine(53, 53, 75, 49); oled.drawLine(55, 55, 73, 52);
      break;
    case SURPRISED:
      oled.drawEllipse(64, 52, 5, 7);
      break;
    case CURIOUS:
      oled.drawCircle(64, 52, 4);
      break;
    case SLEEPY:
      oled.drawLine(57, 53, 71, 53); oled.drawLine(60, 56, 68, 56);
      break;
    case LOVE:
      oled.drawLine(56, 52, 72, 52); oled.drawLine(59, 55, 69, 55);
      break;
    case CONFUSED:
      oled.drawLine(54, 53, 63, 51); oled.drawLine(63, 51, 74, 54);
      break;
    default:
      oled.drawLine(56, 53, 72, 53);
      break;
  }
}

void drawFace() {
  oled.clearBuffer();
  drawClockHeader();
  if (alarmRinging) {
    oled.setFont(u8g2_font_helvB14_tf);
    oled.drawStr(17, 34, "WAKE UP!");
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(4, 48, "Alarm is ringing");
    oled.drawStr(4, 61, "Touch or SELECT to stop");
    oled.sendBuffer();
    return;
  }
  if (sleeping) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(36, 25, "SLEEP MODE");
    oled.drawStr(96, 25, "zZ");
    oled.drawLine(12, 39, 49, 39);
    oled.drawLine(79, 39, 116, 39);
    oled.drawLine(56, 53, 72, 53);
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(18, 62, "TOUCH OR PRESS A KEY");
    oled.sendBuffer();
    return;
  }

  int leftY = 17, rightY = 17;
  int leftLid = 0, rightLid = 0;
  bool eyesClosed = blinking || mood == SLEEPY;
  if (mood == SAD || mood == CRYING) { leftY = 19; rightY = 19; }
  if (mood == SURPRISED || mood == EXCITED) { leftY = 14; rightY = 14; }
  if (mood == ANGRY) { leftLid = 5; rightLid = 5; }
  if (mood == SAD || mood == CRYING) { leftLid = 3; rightLid = 3; }
  if (mood == SLEEPY) { leftLid = 15; rightLid = 15; }
  drawCubeEye(8, leftY, 43, 30, eyeLookX, eyesClosed, leftLid);
  drawCubeEye(77, rightY, 43, 30, eyeLookX, eyesClosed, rightLid);

  // Angular eyebrows communicate mood clearly on the tiny display.
  if (mood == ANGRY) {
    oled.drawLine(8, 14, 49, 19); oled.drawLine(78, 19, 119, 14);
  } else if (mood == SAD || mood == CRYING) {
    oled.drawLine(8, 14, 23, 18); oled.drawLine(34, 18, 49, 14);
    oled.drawLine(78, 14, 93, 18); oled.drawLine(104, 18, 119, 14);
  } else if (mood == CURIOUS || mood == CONFUSED) {
    oled.drawLine(10, 13, 49, 13); oled.drawLine(80, 12, 118, 17);
  } else if (mood == SURPRISED || mood == EXCITED) {
    oled.drawLine(10, 12, 48, 12); oled.drawLine(80, 12, 118, 12);
  }
  if (mood == CRYING) {
    int phase = (millis() / 180) % 8;
    drawTear(20, 47, phase);
    drawTear(108, 47, (phase + 4) % 8);
  } else if (mood == SAD && (millis() / 700) % 2 == 0) {
    drawTear(108, 47, 0);
  }
  if (mood == CONFUSED) oled.drawStr(59, 39, "?");
  if (mood == LOVE) { oled.drawPixel(63, 13); oled.drawPixel(65, 13); }
  drawMouth();
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(2, 63, moodNames[mood]);
  oled.sendBuffer();
}

// ---------- Menu / utility screens ----------
void drawMenu() {
  oled.clearBuffer(); drawHeader("ROBOT MENU");
  for (int i = 0; i < 5; i++) {
    int idx = (menuIndex + i) % MENU_COUNT;
    int y = 23 + i * 9;
    if (i == 0) {
      oled.drawBox(0, y - 7, 128, 9); oled.setDrawColor(0);
      oled.drawStr(4, y, ">>" ); oled.drawStr(19, y, menuItems[idx]); oled.setDrawColor(1);
    } else oled.drawStr(19, y, menuItems[idx]);
  }
  oled.sendBuffer();
}
void drawFaceStudio() {
  oled.clearBuffer(); drawHeader("FACE STUDIO");
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(3, 25, "Emotion:"); oled.drawStr(62, 25, moodNames[mood]);
  oled.drawStr(3, 39, "Auto blink:"); oled.drawStr(76, 39, autoBlink ? "ON" : "OFF");
  oled.drawStr(3, 53, "DOWN: emotion"); oled.drawStr(3, 63, "SELECT: blink  HOLD: home");
  oled.sendBuffer();
}
void drawAlarmScreen() {
  oled.clearBuffer(); drawHeader("WAKE-UP ALARM");
  char b[24]; snprintf(b, sizeof(b), "%02d:%02d", alarmHour, alarmMinute);
  oled.setFont(u8g2_font_logisoso24_tf); oled.drawStr(24, 39, b);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(3, 51, alarmEnabled ? "STATUS: ON" : "STATUS: OFF");
  oled.drawStr(3, 62, "DOWN:+hour SELECT:+5m TOUCH:ON/OFF");
  oled.sendBuffer();
}
void drawWeatherScreen() {
  oled.clearBuffer(); drawHeader("NAIROBI WEATHER");
  oled.setFont(u8g2_font_6x10_tf);
  if (weatherValid) {
    char b[24]; snprintf(b, sizeof(b), "Temp: %.1f C", weatherTemp); oled.drawStr(3, 27, b);
    snprintf(b, sizeof(b), "Humidity: %d%%", weatherHumidity); oled.drawStr(3, 39, b);
    oled.drawStr(3, 51, weatherDescription(weatherCode));
  } else oled.drawStr(3, 31, weatherSummary.c_str());
  oled.drawStr(3, 62, "Updated every 15 minutes"); oled.sendBuffer();
}
void drawNotifyScreen() {
  oled.clearBuffer(); drawHeader("NOTIFICATIONS");
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawUTF8(2, 28, notificationMessage.substring(0, 20).c_str());
  if (notificationMessage.length() > 20) oled.drawUTF8(2, 41, notificationMessage.substring(20, 40).c_str());
  if (notificationMessage.length() > 40) oled.drawUTF8(2, 54, notificationMessage.substring(40, 60).c_str());
  oled.drawStr(2, 63, "Send from phone webpage"); oled.sendBuffer();
}
void drawSoundLab() {
  oled.clearBuffer(); drawHeader("SOUND LAB"); oled.setFont(u8g2_font_6x10_tf);
  const char* names[] = {"Happy chirp", "Alert tone", "Three-note tune"};
  oled.drawStr(4, 28, "Selected sound:"); oled.drawStr(4, 43, names[soundIndex]);
  oled.drawStr(4, 61, "DOWN: choose SELECT: play"); oled.sendBuffer();
}
void drawLedLab() {
  oled.clearBuffer(); drawHeader("LED LAB"); oled.setFont(u8g2_font_6x10_tf);
  const char* names[] = {"Alternating", "Both ON", "Both OFF", "Fast flash"};
  oled.drawStr(4, 29, "Pattern:"); oled.drawStr(4, 43, names[ledMode]);
  oled.drawStr(4, 61, "DOWN: change SELECT: run/stop"); oled.sendBuffer();
}
void drawGame() {
  oled.clearBuffer(); drawHeader("REACTION GAME"); oled.setFont(u8g2_font_6x10_tf);
  if (gameState == 0) { oled.drawStr(4, 29, "SELECT to start"); oled.drawStr(4, 44, "Wait for GO, then touch"); }
  else if (gameState == 1) oled.drawStr(4, 37, "Wait... do not touch!");
  else if (gameState == 2) { oled.setFont(u8g2_font_helvB14_tf); oled.drawStr(43, 40, "GO!"); }
  else if (gameState == 3) { char b[24]; snprintf(b, sizeof(b), "%lu ms", gameResult); oled.drawStr(4, 29, "Your reaction:"); oled.setFont(u8g2_font_helvB14_tf); oled.drawStr(4, 51, b); }
  else { oled.drawStr(4, 34, "Too early!"); oled.drawStr(4, 52, "SELECT to retry"); }
  oled.sendBuffer();
}
void drawStatus() {
  oled.clearBuffer(); drawHeader("SYSTEM STATUS"); oled.setFont(u8g2_font_5x7_tf);
  struct tm ti; char d[20] = "Time not synced", t[20] = "--:--:-- EAT";
  if (readLocalTime(ti)) { strftime(d, sizeof(d), "%d/%m/%Y", &ti); strftime(t, sizeof(t), "%H:%M:%S EAT", &ti); }
  oled.drawStr(2, 22, "Nairobi, Kenya"); oled.drawStr(2, 32, d); oled.drawStr(2, 42, t);
  oled.drawStr(2, 52, WiFi.status() == WL_CONNECTED ? "Wi-Fi: connected" : "Wi-Fi: offline");
  char up[24]; snprintf(up, sizeof(up), "Uptime: %lus", millis() / 1000); oled.drawStr(2, 62, up); oled.sendBuffer();
}
void drawSettings() {
  oled.clearBuffer(); drawHeader("SETTINGS"); oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(3, 27, "Auto blink:"); oled.drawStr(85, 27, autoBlink ? "ON" : "OFF");
  oled.drawStr(3, 41, "Sleep after 60 sec idle");
  oled.drawStr(3, 55, "DOWN: next emotion"); oled.drawStr(3, 63, "SELECT: blink  HOLD: home"); oled.sendBuffer();
}
void drawCurrentScreen() {
  if (screen == HOME) drawFace();
  else if (screen == MENU) drawMenu();
  else if (screen == FACE_STUDIO) drawFaceStudio();
  else if (screen == ALARM_SCREEN) drawAlarmScreen();
  else if (screen == WEATHER_SCREEN) drawWeatherScreen();
  else if (screen == NOTIFY_SCREEN) drawNotifyScreen();
  else if (screen == SOUND_LAB) drawSoundLab();
  else if (screen == LED_LAB) drawLedLab();
  else if (screen == GAMES) drawGame();
  else if (screen == STATUS_SCREEN) drawStatus();
  else if (screen == SETTINGS) drawSettings();
}

// ---------- Menu actions and sound / LED behaviour ----------
void nextMood() {
  mood = static_cast<Mood>((static_cast<int>(mood) + 1) % MOOD_COUNT);
  const int notes[MOOD_COUNT] = {900, 1450, 500, 350, 1200, 1750, 400, 1600, 450, 750, 1850};
  beep(notes[mood], 65);
}
void playSelectedSound() {
  if (soundIndex == 0) beep(1500, 100);
  else if (soundIndex == 1) beep(650, 260);
  else { beep(900, 90); } // melody is kept non-blocking; subsequent notes handled below
}
void chooseMenuItem() {
  switch (menuIndex) {
    case 0: screen = HOME; break;
    case 1: screen = FACE_STUDIO; break;
    case 2: screen = ALARM_SCREEN; break;
    case 3: screen = WEATHER_SCREEN; if (WiFi.status() == WL_CONNECTED && millis() - lastWeatherFetch > 30000) fetchWeather(); break;
    case 4: screen = NOTIFY_SCREEN; break;
    case 5: screen = SOUND_LAB; break;
    case 6: screen = LED_LAB; ledLabEnabled = true; break;
    case 7: screen = GAMES; gameState = 0; break;
    case 8: screen = STATUS_SCREEN; break;
    case 9: screen = SETTINGS; break;
  }
  beep(1100, 35);
}
void goHome() { screen = HOME; sleeping = false; lastActivity = millis(); beep(900, 30); }
void startGame() { gameState = 1; gameWait = random(1500, 5000); gameStarted = millis(); beep(700, 45); }

// ---------- Phone/laptop web dashboard ----------
void handleRoot() {
  String page = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Smart Desk</title><style>body{font-family:Arial;max-width:520px;margin:20px auto;padding:14px}section{padding:14px;margin:12px 0;background:#f1f3f5;border-radius:12px}input,button{padding:10px;margin:5px;font-size:16px;max-width:95%}</style></head><body><h2>ESP32 Smart Desk</h2><section><h3>Alarm</h3><form action="/alarm"><label>Hour (0-23)</label><br><input type="number" name="h" min="0" max="23" required><br><label>Minute (0-59)</label><br><input type="number" name="m" min="0" max="59" required><br><button>Save alarm</button></form><form action="/alarmtoggle"><button>Toggle alarm ON/OFF</button></form></section><section><h3>Robot</h3><form action="/mood"><select name="v"><option>NEUTRAL</option><option>HAPPY</option><option>SAD</option><option>ANGRY</option><option>CURIOUS</option><option>SURPRISED</option><option>SLEEPY</option><option>LOVE</option><option>CRYING</option><option>CONFUSED</option><option>EXCITED</option></select><button>Set emotion</button></form><form action="/sleep"><button>Put robot to sleep</button></form><form action="/wake"><button>Wake robot</button></form></section><section><h3>Send a notification</h3><form action="/notify"><input name="msg" maxlength="60" placeholder="Message to display" required><button>Send</button></form></section><section><a href="/status">Device status (JSON)</a><br><a href="/weather">Refresh weather</a><br><a href="/dismiss">Dismiss alarm</a></section></body></html>)HTML";
  server.send(200, "text/html; charset=utf-8", page);
}
void handleAlarmWeb() {
  if (!server.hasArg("h") || !server.hasArg("m")) { server.send(400, "text/plain", "Hour and minute are required"); return; }
  int h = server.arg("h").toInt(), m = server.arg("m").toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) { server.send(400, "text/plain", "Invalid time"); return; }
  alarmHour = h; alarmMinute = m; alarmEnabled = true; saveAlarm();
  server.sendHeader("Location", "/"); server.send(303);
}
void handleMoodWeb() {
  if (server.hasArg("v")) {
    String value = server.arg("v"); value.toUpperCase();
    for (int i = 0; i < MOOD_COUNT; i++) if (value == moodNames[i]) mood = static_cast<Mood>(i);
    screen = HOME; sleeping = false; lastActivity = millis();
  }
  server.sendHeader("Location", "/"); server.send(303);
}
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/alarm", HTTP_GET, handleAlarmWeb);
  server.on("/alarmtoggle", HTTP_GET, [](){ alarmEnabled = !alarmEnabled; saveAlarm(); server.sendHeader("Location", "/"); server.send(303); });
  server.on("/notify", HTTP_GET, [](){
    if (server.hasArg("msg")) { notificationMessage = server.arg("msg"); notificationMessage.replace("\r", " "); notificationMessage.replace("\n", " "); if (notificationMessage.length() > 60) notificationMessage.remove(60); screen = NOTIFY_SCREEN; sleeping = false; lastActivity = millis(); }
    server.sendHeader("Location", "/"); server.send(303);
  });
  server.on("/mood", HTTP_GET, handleMoodWeb);
  server.on("/sleep", HTTP_GET, [](){ sleeping = true; screen = HOME; server.sendHeader("Location", "/"); server.send(303); });
  server.on("/wake", HTTP_GET, [](){ sleeping = false; screen = HOME; lastActivity = millis(); server.sendHeader("Location", "/"); server.send(303); });
  server.on("/dismiss", HTTP_GET, [](){ stopAlarm(); server.send(200, "text/plain", "Alarm dismissed"); });
  server.on("/weather", HTTP_GET, [](){ fetchWeather(); server.send(200, "text/plain", weatherSummary); });
  server.on("/status", HTTP_GET, [](){
    String j = "{";
    struct tm statusTime;
    bool synced = readLocalTime(statusTime);
    j += "\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "offline") + "\",";
    j += "\"time_synced\":" + String(synced ? "true" : "false") + ",";
    j += "\"emotion\":\"" + String(moodNames[mood]) + "\",";
    j += "\"alarm_enabled\":" + String(alarmEnabled ? "true" : "false") + ",";
    j += "\"alarm_time\":\"" + String(alarmHour) + ":" + String(alarmMinute) + "\",";
    j += "\"sleeping\":" + String(sleeping ? "true" : "false") + ",";
    j += "\"alarm_ringing\":" + String(alarmRinging ? "true" : "false") + "}";
    server.send(200, "application/json", j);
  });
  server.begin();
  webServerStarted = true;
}

// ---------- Interaction ----------
void handleTouch() {
  bool active = digitalRead(TOUCH_PIN) == HIGH;
  if (active && !touchWasActive && millis() - lastTouchDebounce > 180) {
    lastTouchDebounce = millis();
    noteActivity();
    if (alarmRinging) stopAlarm();
    else if (screen == HOME) nextMood();
    else if (screen == ALARM_SCREEN) { alarmEnabled = !alarmEnabled; saveAlarm(); beep(1100, 50); }
    else if (screen == FACE_STUDIO) nextMood();
    else if (screen == SOUND_LAB) playSelectedSound();
    else if (screen == LED_LAB) { ledMode = (ledMode + 1) % 4; }
    else if (screen == GAMES) {
      if (gameState == 2) { gameResult = millis() - gameStarted; gameState = 3; beep(1600, 70); }
      else if (gameState == 1) { gameState = 4; beep(300, 150); }
      else startGame();
    } else goHome();
  }
  touchWasActive = active;
}
void handleButtons() {
  updateButton(downBtn);
  updateButton(selectBtn);
  if (downBtn.pressEvent || selectBtn.pressEvent) noteActivity();

  if (alarmRinging) {
    if (selectBtn.pressEvent || selectBtn.longEvent) stopAlarm();
    return;
  }
  if (sleeping) {
    if (downBtn.pressEvent || selectBtn.pressEvent) noteActivity();
    return;
  }
  if (selectBtn.longEvent) { goHome(); return; }

  if (downBtn.pressEvent) {
    switch (screen) {
      case HOME: nextMood(); break;
      case MENU: menuIndex = (menuIndex + 1) % MENU_COUNT; beep(750, 25); break;
      case FACE_STUDIO: nextMood(); break;
      case ALARM_SCREEN: alarmHour = (alarmHour + 1) % 24; saveAlarm(); break;
      case SOUND_LAB: soundIndex = (soundIndex + 1) % 3; beep(750, 25); break;
      case LED_LAB: ledMode = (ledMode + 1) % 4; break;
      case GAMES: if (gameState == 0 || gameState == 3 || gameState == 4) startGame(); break;
      case SETTINGS: nextMood(); break;
      default: break;
    }
  }
  if (selectBtn.pressEvent) {
    switch (screen) {
      case HOME: screen = MENU; menuIndex = 0; beep(1000, 30); break;
      case MENU: chooseMenuItem(); break;
      case FACE_STUDIO: autoBlink = !autoBlink; break;
      case ALARM_SCREEN: alarmMinute = (alarmMinute + 5) % 60; saveAlarm(); break;
      case SOUND_LAB: playSelectedSound(); break;
      case LED_LAB: ledLabEnabled = !ledLabEnabled; if (!ledLabEnabled) { digitalWrite(LED1, LOW); digitalWrite(LED2, LOW); } break;
      case GAMES:
        if (gameState == 0 || gameState == 3 || gameState == 4) startGame();
        else if (gameState == 1) { gameState = 4; beep(300, 150); }
        else if (gameState == 2) { gameResult = millis() - gameStarted; gameState = 3; beep(1600, 70); }
        break;
      case STATUS_SCREEN: case WEATHER_SCREEN: case NOTIFY_SCREEN: goHome(); break;
      case SETTINGS: autoBlink = !autoBlink; break;
      default: goHome(); break;
    }
  }
}

void updateAnimations() {
  const unsigned long now = millis();
  if (screen == HOME && !sleeping && !alarmRinging) {
    if (autoBlink && !blinking && now - lastBlink >= (mood == SLEEPY ? 1200UL : 2800UL)) {
      blinking = true; blinkStarted = now;
    }
    if (blinking && now - blinkStarted >= 150) { blinking = false; lastBlink = now; }
    if (now - lastLook >= 850) {
      eyeLookX = random(-5, 6);
      lastLook = now;
    }
    // Enter sleep after one minute with no button, touch or web activity.
    if (now - lastActivity >= 60000UL) {
      sleeping = true;
      mood = SLEEPY;
      blinking = false;
      digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
    }
  } else blinking = false;

  if (screen == GAMES && gameState == 1 && now - gameStarted >= gameWait) {
    gameState = 2; gameStarted = now; beep(1800, 80);
  }
}
void updateLEDs() {
  if (alarmRinging) return;
  if (!ledLabEnabled || screen != LED_LAB || sleeping) {
    if (!alarmRinging) { digitalWrite(LED1, LOW); digitalWrite(LED2, LOW); }
    return;
  }
  unsigned long interval = ledMode == 3 ? 100UL : 350UL;
  if (millis() - lastLedTick < interval) return;
  lastLedTick = millis(); ledPhase = !ledPhase;
  if (ledMode == 0) { digitalWrite(LED1, ledPhase); digitalWrite(LED2, !ledPhase); }
  else if (ledMode == 1) { digitalWrite(LED1, HIGH); digitalWrite(LED2, HIGH); }
  else if (ledMode == 2) { digitalWrite(LED1, LOW); digitalWrite(LED2, LOW); }
  else { digitalWrite(LED1, ledPhase); digitalWrite(LED2, ledPhase); }
}

void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT); // common touch modules output HIGH on touch
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(LED1, OUTPUT); pinMode(LED2, OUTPUT); pinMode(BUZZER, OUTPUT);
  digitalWrite(LED1, LOW); digitalWrite(LED2, LOW);
  Wire.begin(21, 22);
  oled.begin();
  oled.setContrast(200);
  prefs.begin("robotdesk", false);
  loadAlarm();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  randomSeed(esp_random());
  lastWiFiAttempt = millis() - 30000UL;
  lastActivity = millis(); lastBlink = millis(); lastLook = millis();
  maintainWiFiTime();
  beep(1300, 80);
  drawCurrentScreen();
}

void loop() {
  maintainWiFiTime();
  if (WiFi.status() == WL_CONNECTED) {
    if (!webServerStarted) setupWebServer();
    server.handleClient();
  }
  handleButtons();
  handleTouch();
  updateAnimations();
  updateLEDs();
  checkAlarm();
  updateAlarmOutputs();

  if (WiFi.status() == WL_CONNECTED && millis() - lastWeatherFetch >= 900000UL) {
    fetchWeather();
  }
  // Rotate secondary information pages only when user is not interacting with menus.
  if (screen == HOME && !sleeping && !alarmRinging && millis() - lastAutoPage >= 20000UL) {
    lastAutoPage = millis();
    // Keep the robot face as the main page; weather/time are opened from the menu.
  }
  if (millis() - lastFrame >= 70) {
    lastFrame = millis();
    drawCurrentScreen();
  }
  delay(3);
}
