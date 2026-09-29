/*
 * AI-Thinker ESP32-CAM Guard
 * Firmware version: 1.1.0
 * Target: AI Thinker ESP32-CAM, Arduino-ESP32 2.0.17
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <ctype.h>
#include "Config.h"
#include "CameraService.h"
#include "TelegramService.h"
#include "MotionDetector.h"
#include "BluetoothPresence.h"
#include "ScheduleManager.h"
#include "WebConfigServer.h"

static const char *FIRMWARE_VERSION = "1.1.0";
static const uint32_t WIFI_TIMEOUT_MS = 60000;
static const uint32_t MOTION_SAMPLE_INTERVAL_MS = 300;
static const uint32_t TELEGRAM_POLL_INTERVAL_MS = 1800;

ConfigStore configStore;
AppConfig appConfig;
CameraService cameraService;
TelegramService telegramService;
MotionDetector motionDetector;
BluetoothPresence bluetoothPresence;
ScheduleManager scheduleManager;
WebConfigServer webConfigServer;

int64_t telegramUpdateOffset = 0;
uint32_t lastMotionSampleMs = 0;
uint32_t lastPeriodicCaptureMs = 0;
uint32_t lastTelegramPollMs = 0;
uint32_t lastPendingFlushMs = 0;
uint32_t lastMotionActionMs = 0;
bool motionActionStarted = false;
bool previousOwnerPresent = false;
bool ownerStateInitialized = false;
bool wasQuiet = false;
bool otaStarted = false;
bool cameraReady = false;
bool telegramConfigurationWarningPrinted = false;

uint32_t pendingMotionPhotos = 0;
uint32_t pendingManualPhotos = 0;
uint32_t pendingPeriodicPhotos = 0;
uint32_t pendingOwnerArrivals = 0;
uint32_t pendingOwnerDepartures = 0;

static String makeAccessPointName() {
  String mac = WiFi.macAddress();
  String suffix;
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    if (isxdigit(static_cast<unsigned char>(c))) suffix += static_cast<char>(toupper(c));
  }
  if (suffix.length() > 6) suffix = suffix.substring(suffix.length() - 6);
  if (suffix.isEmpty()) suffix = "000000";
  return "Guard-" + suffix;
}

static String generateDevicePassword() {
  static const char alphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@$%";
  String password;
  password.reserve(20);
  for (uint8_t i = 0; i < 20; ++i) {
    password += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  return password;
}

static bool prepareDevicePassword() {
  if (appConfig.devicePassword.length() >= 12 &&
      appConfig.devicePassword.length() <= 63) return true;

  appConfig.devicePassword = generateDevicePassword();
  appConfig.otaEnabled = false;
  if (!configStore.save(appConfig)) {
    Serial.println("ОШИБКА: не удалось сохранить пароль устройства в NVS; сеть не запускается.");
    return false;
  }
  Serial.printf("Новый пароль устройства (admin, AP, OTA): %s\n",
                appConfig.devicePassword.c_str());
  Serial.println("Сохраните его: повторно он не печатается.");
  return true;
}

static void queuePhotos(const String &kind, uint32_t count) {
  if (kind == "motion") pendingMotionPhotos += count;
  else if (kind == "manual") pendingManualPhotos += count;
  else if (kind == "periodic") pendingPeriodicPhotos += count;
}

static bool canSendNow() {
  return WiFi.status() == WL_CONNECTED &&
         telegramService.isConfigured() &&
         !scheduleManager.isQuietNow();
}

static void notifyOrQueue(const String &text, bool ownerArrival) {
  if (!telegramService.isConfigured()) return;
  if (canSendNow() && telegramService.sendMessage(text)) return;
  if (ownerArrival) ++pendingOwnerArrivals;
  else ++pendingOwnerDepartures;
}

static void sendPhotoBurst(uint8_t count, const String &caption, const String &kind) {
  if (!telegramService.isConfigured()) {
    if (!telegramConfigurationWarningPrinted) {
      Serial.println("Съёмка/отправка пропущена: не заданы токен бота и chat ID.");
      telegramConfigurationWarningPrinted = true;
    }
    return;
  }
  if (!cameraReady) {
    telegramService.sendMessage("Камера не обнаружена; снимки недоступны. Проверьте модуль и шлейф.");
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    if (!canSendNow()) {
      queuePhotos(kind, count - i);
      return;
    }

    camera_fb_t *frame = cameraService.takePhoto();
    if (frame == nullptr) {
      queuePhotos(kind, count - i);
      return;
    }

    String photoCaption = caption;
    if (count > 1) {
      photoCaption += " (" + String(i + 1) + "/" + String(count) + ")";
    }
    const bool sent = telegramService.sendPhoto(frame, photoCaption);
    cameraService.releasePhoto(frame);
    if (!sent) {
      queuePhotos(kind, count - i);
      return;
    }
    if (i + 1 < count) delay(700);
  }
}

static void flushPendingWork() {
  if (!canSendNow()) return;
  if (!cameraReady &&
      (pendingMotionPhotos || pendingManualPhotos || pendingPeriodicPhotos)) return;

  const bool hasMessages = pendingOwnerArrivals || pendingOwnerDepartures;
  const bool hasPhotos = pendingMotionPhotos || pendingManualPhotos || pendingPeriodicPhotos;
  if (!hasMessages && !hasPhotos) return;

  const uint32_t now = millis();
  if (now - lastPendingFlushMs < 60000) return;
  lastPendingFlushMs = now;

  String summary = "Отложенные события за время недоступности/режима тишины:";
  if (pendingOwnerArrivals) summary += "\n«Капитан на постике»: " + String(pendingOwnerArrivals);
  if (pendingOwnerDepartures) summary += "\n«Сторож бдит»: " + String(pendingOwnerDepartures);
  if (pendingMotionPhotos) summary += "\nСнимки по движению: " + String(pendingMotionPhotos);
  if (pendingManualPhotos) summary += "\nСнимки GetCapture: " + String(pendingManualPhotos);
  if (pendingPeriodicPhotos) summary += "\nПлановые снимки: " + String(pendingPeriodicPhotos);
  if (hasPhotos) {
    summary += "\nБез SD-карты изображения не сохранялись; ниже будет отправлен один актуальный кадр.";
  }

  if (!telegramService.sendMessage(summary)) return;
  pendingOwnerArrivals = 0;
  pendingOwnerDepartures = 0;

  if (hasPhotos && canSendNow()) {
    camera_fb_t *frame = cameraService.takePhoto();
    if (frame != nullptr) {
      const bool sent = telegramService.sendPhoto(frame, "Актуальный кадр после режима тишины");
      cameraService.releasePhoto(frame);
      if (sent) {
        pendingMotionPhotos = 0;
        pendingManualPhotos = 0;
        pendingPeriodicPhotos = 0;
      }
    }
  }
}

static bool startNetworkAndPortal() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  delay(100);  // Start the RF subsystem before requesting hardware random bytes.
  if (!prepareDevicePassword()) return false;

  bool connected = false;
  if (!appConfig.wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(appConfig.wifiSsid.c_str(), appConfig.wifiPassword.c_str());
    Serial.printf("Подключение к Wi-Fi (ожидание до %lu секунд)...\n",
                  static_cast<unsigned long>(WIFI_TIMEOUT_MS / 1000));
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_TIMEOUT_MS) {
      delay(250);
    }
    connected = WiFi.status() == WL_CONNECTED;
  }

  if (connected) {
    Serial.print("Wi-Fi подключён, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    const String apName = makeAccessPointName();
    if (WiFi.softAP(apName.c_str(), appConfig.devicePassword.c_str())) {
      Serial.printf("Режим настройки: SSID=%s, адрес http://192.168.4.1/\n",
                    apName.c_str());
    } else {
      Serial.println("Не удалось запустить точку доступа.");
    }
  }

  webConfigServer.begin(configStore, appConfig);
  if (connected && appConfig.otaEnabled) {
    ArduinoOTA.setHostname("ai-thinker-guard");
    ArduinoOTA.setPassword(appConfig.devicePassword.c_str());
    ArduinoOTA.onStart([]() { Serial.println("Началась OTA-прошивка."); });
    ArduinoOTA.onEnd([]() { Serial.println("\nOTA-прошивка завершена."); });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Ошибка OTA: %u\n", static_cast<unsigned>(error));
    });
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.println("OTA включена (порт 3232); пароль совпадает с паролем устройства.");
  } else if (connected) {
    Serial.println("OTA выключена в настройках.");
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nAI-Thinker Guard firmware %s\n", FIRMWARE_VERSION);

  Serial.println("[init] NVS...");
  if (!configStore.begin()) {
    Serial.println("Ошибка открытия NVS. Перезапустите устройство.");
    while (true) delay(1000);
  }
  configStore.load(appConfig);
  telegramUpdateOffset = configStore.loadUpdateOffset();

  Serial.println("[init] Камера...");
  cameraReady = cameraService.begin();
  if (!cameraReady) {
    Serial.println("Камера недоступна; запуск Wi-Fi и страницы настройки продолжается.");
  } else {
    Serial.println("[init] Камера готова.");
  }

  Serial.println("[init] Сеть и веб-конфигурация...");
  if (!startNetworkAndPortal()) {
    Serial.println("Сеть не запущена, так как не удалось сохранить пароль устройства.");
    while (true) delay(1000);
  }
  Serial.println("[init] Telegram...");
  telegramService.configure(appConfig.telegramToken, appConfig.chatId);
  if (!telegramService.isConfigured()) {
    Serial.print("Telegram отключён: ");
    Serial.println(telegramService.configurationProblem());
  }
  Serial.println("[init] NTP/расписание...");
  scheduleManager.begin(appConfig);
  Serial.println("[init] BLE...");
  bluetoothPresence.begin(appConfig.ownerMac);
  Serial.println("[init] BLE обработан.");
  previousOwnerPresent = bluetoothPresence.isPresent();
  ownerStateInitialized = true;
  wasQuiet = scheduleManager.isQuietNow();
  lastPeriodicCaptureMs = millis();
  lastPendingFlushMs = millis() - 60000;

  Serial.println("Инициализация завершена.");
}

void loop() {
  webConfigServer.handleClient();
  if (otaStarted) ArduinoOTA.handle();

  const uint32_t now = millis();
  const bool quietNow = scheduleManager.isQuietNow();
  if (wasQuiet && !quietNow) lastPendingFlushMs = now - 60000;
  wasQuiet = quietNow;

  const bool ownerPresent = bluetoothPresence.isPresent();
  if (!ownerStateInitialized) {
    previousOwnerPresent = ownerPresent;
    ownerStateInitialized = true;
  } else if (ownerPresent != previousOwnerPresent) {
    previousOwnerPresent = ownerPresent;
    if (ownerPresent) {
      notifyOrQueue("Капитан на постике", true);
    } else {
      notifyOrQueue("Сторож бдит", false);
    }
  }

  if (WiFi.status() == WL_CONNECTED && telegramService.isConfigured() &&
      now - lastTelegramPollMs >= TELEGRAM_POLL_INTERVAL_MS) {
    lastTelegramPollMs = now;
    String command;
    const int64_t oldOffset = telegramUpdateOffset;
    if (telegramService.pollCommand(telegramUpdateOffset, command)) {
      if (command == "getcapture") {
        sendPhotoBurst(appConfig.manualCount, "Снимок по запросу GetCapture", "manual");
      }
    }
    if (telegramUpdateOffset != oldOffset) {
      configStore.saveUpdateOffset(telegramUpdateOffset);
    }
  }

  if (cameraReady && telegramService.isConfigured() &&
      now - lastMotionSampleMs >= MOTION_SAMPLE_INTERVAL_MS) {
    lastMotionSampleMs = now;
    camera_fb_t *frame = cameraService.captureMotionFrame();
    if (frame != nullptr) {
      const bool movementDetected = motionDetector.detect(frame->buf, frame->len);
      cameraService.releaseMotionFrame(frame);
      const uint32_t timeoutMs = static_cast<uint32_t>(appConfig.timeoutSeconds) * 1000UL;
      if (movementDetected && !ownerPresent &&
          (!motionActionStarted || now - lastMotionActionMs >= timeoutMs)) {
        motionActionStarted = true;
        lastMotionActionMs = now;
        sendPhotoBurst(appConfig.motionCount, "Обнаружено движение", "motion");
      }
    }
  }

  const uint32_t periodicMs = static_cast<uint32_t>(appConfig.periodicMinutes) * 60000UL;
  if (cameraReady && telegramService.isConfigured() &&
      now - lastPeriodicCaptureMs >= periodicMs) {
    lastPeriodicCaptureMs = now;
    sendPhotoBurst(1, "Плановый снимок", "periodic");
  }

  flushPendingWork();
  delay(5);
}