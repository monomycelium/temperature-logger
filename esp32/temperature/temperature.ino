#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_sleep.h>
#include <esp_attr.h>
#include <sys/time.h>
#include <math.h>

#include "arduino_secrets.h"

// ----- Hardware -----
// TTP223 must be in momentary, active-HIGH mode.
constexpr uint8_t SENSOR_PIN = 0;  // DS18B20 data (pull up to 3V3)
constexpr uint8_t TOUCH_PIN  = 3;  // TTP223 SIG, active HIGH
constexpr uint8_t OLED_SDA   = 1;  // SSD1306 SDA
constexpr uint8_t OLED_SCL   = 2;  // SSD1306 SCL
constexpr uint8_t OLED_ADDR  = 0x3C;

// ----- Configuration -----
constexpr char UPLOAD_URL[] = "http://192.168.18.197:6767/temperature";
constexpr uint64_t UPLOAD_INTERVAL_US = 3ULL * 60ULL * 1000000ULL; // 3 minutes
constexpr uint32_t QUICK_VIEW_MS = 5000;
constexpr uint32_t LONG_TOUCH_MS = 1500;
constexpr uint32_t LIVE_REFRESH_MS = 2000;
constexpr uint32_t WIFI_TIMEOUT_MS = 12000;
constexpr uint32_t HTTP_TIMEOUT_MS = 3000;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t STATE_MAGIC = 0xC3002030;

OneWire oneWire(SENSOR_PIN);
DallasTemperature sensors(&oneWire);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WebServer webServer(80);

// Retained across deep sleep, but not resets from unplugging the battery.
struct PersistentState {
  uint32_t magic;
  uint64_t startedUs;
  uint64_t nextUploadUs;
  uint64_t lastSuccessUs;
  uint8_t lastAttempt;  // 0: never, 1: succeeded, 2: failed
};
RTC_DATA_ATTR PersistentState state;

float latestTemperature = DEVICE_DISCONNECTED_C;
uint32_t latestTemperatureMs = 0;
bool oledReady = false;
bool webServerRunning = false;
bool exitLiveMode = false;
uint32_t lastWiFiRetryMs = 0;

// ----- Clock and sensor -----
uint64_t clockUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

uint64_t uptimeSeconds() {
  return (clockUs() - state.startedUs) / 1000000ULL;
}

void formatDuration(char *buffer, size_t size, uint64_t seconds) {
  if (seconds < 60)
    snprintf(buffer, size, "%llus", (unsigned long long)seconds);
  else if (seconds < 3600)
    snprintf(buffer, size, "%llum", (unsigned long long)(seconds / 60));
  else if (seconds < 86400)
    snprintf(buffer, size, "%lluh%llum",
             (unsigned long long)(seconds / 3600),
             (unsigned long long)((seconds / 60) % 60));
  else
    snprintf(buffer, size, "%llud%lluh",
             (unsigned long long)(seconds / 86400),
             (unsigned long long)((seconds / 3600) % 24));
}

bool temperatureValid(float t) {
  return isfinite(t) && t != DEVICE_DISCONNECTED_C &&
         t >= -55.0f && t <= 125.0f;
}

float readTemperature() {
  sensors.requestTemperatures();  // Blocking, up to ~750 ms at 12-bit resolution.
  latestTemperature = sensors.getTempCByIndex(0);
  latestTemperatureMs = millis();

  if (temperatureValid(latestTemperature))
    Serial.printf("[Sensor] %.2f C\n", latestTemperature);
  else
    Serial.println("[Sensor] Read failed");

  return latestTemperature;
}

// ----- OLED -----
bool initOled() {
  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  if (!oledReady) Serial.println("[OLED] Initialization failed");
  return oledReady;
}

void drawDisplay(bool live) {
  if (!oledReady) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);

  if (live) {
    oled.print("LIVE");
    String ip = WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString() : String("No WiFi");
    // Default 6-pixel-wide GFX font: align IP with the right edge.
    const int16_t x = max(28, 128 - (int)ip.length() * 6);
    oled.setCursor(x, 0);
    oled.print(ip);
  } else {
    oled.print("TEMPERATURE");
  }

  oled.setCursor(0, 16);
  if (temperatureValid(latestTemperature)) {
    oled.setTextSize(3);
    oled.print(latestTemperature, 2);
    oled.setTextSize(1);
    oled.setCursor(105, 29);
    oled.print("C");
  } else {
    oled.setTextSize(2);
    oled.print("SENSOR ERR");
  }

  oled.drawFastHLine(0, 43, 128, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 46);

  char duration[20];
  formatDuration(duration, sizeof(duration), uptimeSeconds());
  oled.print("Up ");
  oled.print(duration);

  oled.setCursor(0, 56);
  if (state.lastSuccessUs != 0) {
    formatDuration(duration, sizeof(duration),
                   (clockUs() - state.lastSuccessUs) / 1000000ULL);
    oled.print("Last OK ");
    oled.print(duration);
    if (state.lastAttempt == 2) oled.print(" !");
  } else {
    oled.print("Last OK never");
  }

  oled.display();
}

void showConnecting() {
  if (!oledReady) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("LIVE");
  oled.setCursor(0, 25);
  oled.print("Connecting Wi-Fi...");
  oled.display();
}

void turnOffOled() {
  if (oledReady) oled.ssd1306_command(SSD1306_DISPLAYOFF);
}

// ----- Wi-Fi and outgoing HTTP -----
bool ensureWifiConnected(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SECRET_SSID, SECRET_PASS);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startMs < timeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected: %s (RSSI %d dBm)\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.println("[WiFi] Connection failed/timed out");
  return false;
}

bool postTemperature(float temperature) {
  bool success = false;

  if (ensureWifiConnected(WIFI_TIMEOUT_MS)) {
    WiFiClient client;
    HTTPClient http;
    if (http.begin(client, UPLOAD_URL)) {
      http.setConnectTimeout(HTTP_TIMEOUT_MS);
      http.setTimeout(HTTP_TIMEOUT_MS);
      http.addHeader("Content-Type", "application/json");

      String body = "{\"temperature\":" + String(temperature, 2) + "}";
      const int status = http.POST(body);
      success = status >= 200 && status < 300;
      Serial.printf("[HTTP] POST %.2f C: %d (%s)\n",
                    temperature, status, success ? "OK" : "FAILED");
      if (status < 0)
        Serial.printf("[HTTP] %s\n", http.errorToString(status).c_str());
      http.end();
    } else {
      Serial.println("[HTTP] Could not create request");
    }
  }

  state.lastAttempt = success ? 1 : 2;
  if (success) state.lastSuccessUs = clockUs();
  return success;
}

// Keep the upload schedule on the retained clock, not millis().
// Missing a deadline never creates a backlog of duplicate uploads.
void uploadIfDue(bool keepWifiOn) {
  const uint64_t now = clockUs();
  if (now < state.nextUploadUs) return;

  // Advance first, so even failures won't trigger an immediate retry.
  const uint64_t intervals =
      (now - state.nextUploadUs) / UPLOAD_INTERVAL_US + 1ULL;
  state.nextUploadUs += intervals * UPLOAD_INTERVAL_US;

  const float temperature = readTemperature();
  if (temperatureValid(temperature)) {
    postTemperature(temperature);
  } else {
    state.lastAttempt = 2;
    Serial.println("[HTTP] Skipped: invalid sensor reading");
  }

  if (!keepWifiOn) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

// ----- Incoming HTTP, only available in LIVE mode -----
void registerRoutes() {
  webServer.on("/temperature", HTTP_GET, []() {
    if (!temperatureValid(latestTemperature)) {
      webServer.send(503, "application/json",
                     "{\"error\":\"sensor unavailable\"}");
      return;
    }
    String body = "{\"temperature\":" +
                  String(latestTemperature, 2) + ",\"unit\":\"C\"}";
    webServer.send(200, "application/json", body);
  });

  webServer.on("/status", HTTP_GET, []() {
    char number[24];
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)uptimeSeconds());
    String body = "{\"mode\":\"live\",\"uptime_seconds\":";
    body += number;
    body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    body += ",\"rssi_dbm\":" + String(WiFi.RSSI());
    body += ",\"last_upload_ok\":";
    body += (state.lastAttempt == 1 ? "true" : "false");
    body += ",\"seconds_since_last_success\":";
    if (state.lastSuccessUs != 0) {
      snprintf(number, sizeof(number), "%llu",
               (unsigned long long)
               ((clockUs() - state.lastSuccessUs) / 1000000ULL));
      body += number;
    } else {
      body += "null";
    }
    body += "}";
    webServer.send(200, "application/json", body);
  });

  webServer.on("/sleep", HTTP_POST, []() {
    webServer.send(200, "text/plain", "Going to sleep\n");
    exitLiveMode = true; // Sleep only after the HTTP response is sent.
  });

  webServer.onNotFound([]() {
    webServer.send(404, "text/plain", "Not found\n");
  });
}

// ----- Touch handling -----
// The TTP223 HIGH signal remains active while the finger is present.
// Measure hold duration after deep-sleep wake. A short touch returns false.
bool wasLongTouch() {
  if (digitalRead(TOUCH_PIN) == LOW) return false;
  const uint32_t started = millis();
  while (digitalRead(TOUCH_PIN) == HIGH) {
    if (millis() - started >= LONG_TOUCH_MS) return true;
    delay(15);
  }
  return false;
}

void runQuickView() {
  Serial.println("[Mode] Quick view");
  const uint32_t start = millis();
  while (millis() - start < QUICK_VIEW_MS) {
    uploadIfDue(false);  // Still upload if the deadline arrives.
    drawDisplay(false);
    delay(500);
  }
  turnOffOled();
}

void runLiveMode() {
  Serial.println("[Mode] LIVE: refresh every 2 s; upload every 60 s");
  showConnecting();

  exitLiveMode = false;
  webServerRunning = false;
  lastWiFiRetryMs = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  registerRoutes();

  uint32_t lastDisplayMs = 0;
  bool touchArmed = false;
  uint32_t releasedAtMs = 0;

  while (!exitLiveMode) {
    const bool connected = WiFi.status() == WL_CONNECTED;

    if (connected && !webServerRunning) {
      webServer.begin();
      webServerRunning = true;
      Serial.printf("[LIVE] HTTP server: http://%s/\n",
                    WiFi.localIP().toString().c_str());
      drawDisplay(true);
    } else if (!connected && webServerRunning) {
      webServer.stop();
      webServerRunning = false;
      Serial.println("[LIVE] Wi-Fi disconnected");
    }

    if (!connected && millis() - lastWiFiRetryMs >= WIFI_RETRY_MS) {
      lastWiFiRetryMs = millis();
      WiFi.reconnect();
    }

    if (webServerRunning) webServer.handleClient();
    if (exitLiveMode) break;

    // Only re-arm after the original long touch has been released.
    if (digitalRead(TOUCH_PIN) == LOW) {
      if (releasedAtMs == 0) releasedAtMs = millis();
      if (millis() - releasedAtMs >= 75) touchArmed = true;
    } else {
      releasedAtMs = 0;
      if (touchArmed) {
        Serial.println("[LIVE] Touch to sleep");
        exitLiveMode = true;
        break;
      }
    }

    uploadIfDue(true);
    if (exitLiveMode) break;

    if (millis() - latestTemperatureMs >= LIVE_REFRESH_MS) {
      readTemperature();
    }
    if (millis() - lastDisplayMs >= LIVE_REFRESH_MS) {
      drawDisplay(true);
      lastDisplayMs = millis();
    }

    delay(15);
  }

  if (webServerRunning) {
    webServer.stop();
    webServerRunning = false;
  }
  delay(120); // Allow final /sleep response to leave the socket.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  turnOffOled();
  Serial.println("[Mode] Leaving LIVE");
}

// ----- Deep sleep -----
void goToSleep() {
  // If TTP223 is still held HIGH, allow it up to 2 s to release.
  // Never wait indefinitely, or scheduled logging could stop.
  const uint32_t start = millis();
  while (digitalRead(TOUCH_PIN) == HIGH &&
         millis() - start < 2000) delay(25);
  delay(75);
  const bool allowTouchWake = digitalRead(TOUCH_PIN) == LOW;

  // If a scheduled upload fell due while waiting for release, do it now.
  uploadIfDue(false);

  const uint64_t now = clockUs();
  const uint64_t sleepUs = state.nextUploadUs > now
      ? state.nextUploadUs - now : 100000ULL;

  const esp_err_t timerError = esp_sleep_enable_timer_wakeup(sleepUs);
  const esp_err_t touchError = allowTouchWake
      ? esp_deep_sleep_enable_gpio_wakeup(
            1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH)
      : ESP_OK;

  if (timerError != ESP_OK || touchError != ESP_OK) {
    Serial.printf("[Sleep] Config error: timer=%d touch=%d\n",
                  (int)timerError, (int)touchError);
    delay(2000);
    ESP.restart();
  }

  Serial.printf("[Sleep] %.1f s, touch wake %s\n",
                (double)sleepUs / 1000000.0,
                allowTouchWake ? "enabled" : "disabled");
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // Native USB must not block without a monitor.

  pinMode(TOUCH_PIN, INPUT);  // TTP223 drives both HIGH and LOW.
  sensors.begin();

  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool resumed = cause == ESP_SLEEP_WAKEUP_TIMER ||
                       cause == ESP_SLEEP_WAKEUP_GPIO;
  const bool touchWake = cause == ESP_SLEEP_WAKEUP_GPIO;
  const uint64_t now = clockUs();

  if (!resumed || state.magic != STATE_MAGIC ||
      state.startedUs > now || state.nextUploadUs < state.startedUs ||
      (state.lastSuccessUs != 0 && state.lastSuccessUs > now)) {
    state.magic = STATE_MAGIC;
    state.startedUs = now;
    state.nextUploadUs = now; // Upload immediately on initial startup.
    state.lastSuccessUs = 0;
    state.lastAttempt = 0;
    Serial.println("[System] Fresh start");
  } else {
    Serial.printf("[System] Wake: %s\n", touchWake ? "touch" : "timer");
  }

  if (touchWake) {
    readTemperature();
    if (initOled()) drawDisplay(false);  // Immediate visual feedback.

    if (wasLongTouch()) {
      runLiveMode();
    } else {
      runQuickView();
    }
  } else {
    // Initial boot: force the OLED off. On deep-sleep timer wakes it
    // remains off without needing to reinitialise the display.
    if (!resumed && initOled()) turnOffOled();
    uploadIfDue(false);
  }

  goToSleep();
}

void loop() {
  // Deep-sleep operation is handled entirely in setup().
}
