/*
 * AI-Thinker ESP32-CAM Guard
 * Firmware version: 1.4.0
 * Target: AI Thinker ESP32-CAM, Arduino-ESP32 2.0.17
 *
 * Принцип отказоустойчивости: Wi-Fi/точка доступа и страница настроек
 * поднимаются первыми и работают всегда. Любой другой модуль (камера, BLE,
 * Telegram, NVS) при ошибке отключается, а не останавливает прошивку.
 * Если модуль вызвал аварийный перезапуск, на следующей загрузке он
 * пропускается (см. SystemHealth).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include "SystemHealth.h"
#include "Config.h"
#include "CameraService.h"
#include "TelegramService.h"
#include "MotionDetector.h"
#include "BluetoothPresence.h"
#include "ScheduleManager.h"
#include "WebConfigServer.h"

static const char *FIRMWARE_VERSION = "1.4.0";
static const uint32_t WIFI_BOOT_TIMEOUT_MS = 20000;     // ожидание Wi-Fi при старте
static const uint32_t WIFI_RETRY_INTERVAL_MS = 30000;   // повтор подключения STA
static const uint32_t WIFI_LOST_AP_DELAY_MS = 30000;    // AP после потери Wi-Fi
static const uint32_t AP_IDLE_SHUTDOWN_MS = 120000;     // выключить AP, когда STA стабилен
static const uint32_t CAMERA_RETRY_INTERVAL_MS = 300000;
static const uint8_t CAMERA_MAX_EMPTY_FRAMES = 5;
static const uint32_t MOTION_SAMPLE_INTERVAL_MS = 300;
static const uint32_t TELEGRAM_POLL_INTERVAL_MS = 3000;
static const uint32_t TELEGRAM_MAX_BACKOFF_MS = 60000;   // пауза опроса без интернета
static const uint32_t WATCHDOG_TIMEOUT_S = 90;

SystemHealth systemHealth;
ConfigStore configStore;
AppConfig appConfig;
CameraService cameraService;
TelegramService telegramService;
MotionDetector motionDetector;
BluetoothPresence bluetoothPresence;
ScheduleManager scheduleManager;
WebConfigServer webConfigServer;
DNSServer dnsServer;

int64_t telegramUpdateOffset = 0;
uint32_t lastMotionSampleMs = 0;
uint32_t lastPeriodicCaptureMs = 0;
uint32_t lastTelegramPollMs = 0;
uint32_t lastPendingFlushMs = 0;
uint32_t lastMotionActionMs = 0;
uint32_t lastCameraAttemptMs = 0;
uint8_t emptyMotionFrames = 0;
uint32_t telegramPollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
bool motionActionStarted = false;
bool previousOwnerPresent = false;
bool ownerStateInitialized = false;
bool wasQuiet = false;
bool otaStarted = false;
bool telegramEnabled = false;
bool telegramConfigurationWarningPrinted = false;
bool devicePasswordSaved = true;
String cameraStatus = "не инициализирована";
String telegramStatus = "не инициализирован";

// Состояние Wi-Fi.
bool bleWanted = false;
bool staConfigured = false;
bool staWasConnected = false;
bool staAttemptPaused = false;
bool apActive = false;
String apName;
uint32_t staLostSinceMs = 0;
uint32_t staConnectedSinceMs = 0;
uint32_t lastStaAttemptMs = 0;
uint32_t lastWifiServiceMs = 0;

uint32_t pendingMotionPhotos = 0;
uint32_t pendingManualPhotos = 0;
uint32_t pendingPeriodicPhotos = 0;
uint32_t pendingOwnerArrivals = 0;
uint32_t pendingOwnerDepartures = 0;

enum class PhotoKind : uint8_t { Motion, Manual, Periodic };

static void serviceSerialConsole();
static void printConfiguration();

// ---------------------------------------------------------------- пароль ---

static void readMac(uint8_t mac[6]) {
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) memset(mac, 0, 6);
}

// Имя точки: Guard-XXXXXX (последние 3 байта MAC в HEX).
static String makeAccessPointName() {
  uint8_t mac[6];
  readMac(mac);
  char name[16];
  snprintf(name, sizeof(name), "Guard-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return name;
}

// Пароль по умолчанию: Guard + 5 цифр из MAC, например Guard95733.
// Удобно вводить со смартфона; нужен только для первичной настройки.
static String defaultDevicePassword() {
  uint8_t mac[6];
  readMac(mac);
  const uint32_t tail = (static_cast<uint32_t>(mac[3]) << 16) |
                        (static_cast<uint32_t>(mac[4]) << 8) | mac[5];
  char password[16];
  snprintf(password, sizeof(password), "Guard%05lu", static_cast<unsigned long>(tail % 100000UL));
  return password;
}

static void prepareDevicePassword() {
  const bool customValid = appConfig.devicePasswordCustom &&
                           isValidDevicePassword(appConfig.devicePassword);
  if (!customValid) {
    appConfig.devicePassword = defaultDevicePassword();
    appConfig.devicePasswordCustom = false;
  }
  Serial.printf("Пароль устройства (admin, AP, OTA): %s%s\n", appConfig.devicePassword.c_str(),
                customValid ? " (задан вручную)" : " (по умолчанию, из MAC)");
}

// ------------------------------------------------------------------ Wi-Fi ---

static const char *wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED: return "подключено";
    case WL_NO_SSID_AVAIL: return "сеть не найдена";
    case WL_CONNECT_FAILED: return "отказ подключения (проверьте пароль)";
    case WL_CONNECTION_LOST: return "соединение потеряно";
    case WL_DISCONNECTED: return "нет подключения";
    case WL_IDLE_STATUS: return "ожидание";
    default: return "нет подключения";
  }
}

static void applyWifiPowerSave() {
  // При одновременной работе Wi-Fi и Bluetooth IDF требует modem sleep.
  // WIFI_PS_NONE + BLEDevice::init() = abort() в coexist
  // ("Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled").
  WiFi.setSleep(bleWanted ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

static void startOtaIfNeeded() {
  if (otaStarted || !appConfig.otaEnabled || WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname("ai-thinker-guard");
  ArduinoOTA.setPassword(appConfig.devicePassword.c_str());
  ArduinoOTA.onStart([]() { Serial.println("Началась OTA-прошивка."); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA-прошивка завершена."); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { feedWatchdog(); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Ошибка OTA: %u\n", static_cast<unsigned>(error));
  });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("OTA включена (порт 3232); пароль совпадает с паролем устройства.");
}

static void startAccessPoint() {
  if (apActive) return;
  WiFi.mode(staConfigured ? WIFI_AP_STA : WIFI_AP);
  applyWifiPowerSave();
  if (apName.isEmpty()) apName = makeAccessPointName();
  bool ok = WiFi.softAP(apName.c_str(), appConfig.devicePassword.c_str());
  webConfigServer.setAccessPointProtected(ok);
  if (!ok) {
    // Открытая точка: тогда веб-панель и для клиентов точки требует логин admin.
    Serial.println("Точка доступа с паролем не запустилась, пробую открытую.");
    ok = WiFi.softAP(apName.c_str());
  }
  if (!ok) {
    Serial.println("Не удалось запустить точку доступа; повтор при следующей проверке.");
    return;
  }
  apActive = true;
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("Точка настройки: SSID=%s, пароль=%s, адрес http://%s/\n",
                apName.c_str(), appConfig.devicePassword.c_str(),
                WiFi.softAPIP().toString().c_str());
}

static void stopAccessPoint() {
  if (!apActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  applyWifiPowerSave();
  apActive = false;
  Serial.println("Wi-Fi стабилен, точка настройки выключена.");
}

static void beginStationAttempt() {
  lastStaAttemptMs = millis();
  WiFi.disconnect(false, false);
  WiFi.begin(appConfig.wifiSsid.c_str(), appConfig.wifiPassword.c_str());
}

static void onStationConnected() {
  staWasConnected = true;
  staConnectedSinceMs = millis();
  staLostSinceMs = 0;
  Serial.print("Wi-Fi подключён, IP: ");
  Serial.println(WiFi.localIP());
  startOtaIfNeeded();
  if (!appConfig.otaEnabled && !otaStarted) Serial.println("OTA выключена в настройках.");
}

static void startNetworkAndPortal() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);  // переподключением управляет serviceWifi()
  applyWifiPowerSave();
  WiFi.mode(WIFI_STA);
  delay(100);  // RF должен стартовать до запроса аппаратных случайных чисел.
  prepareDevicePassword();

  staConfigured = !appConfig.wifiSsid.isEmpty();
  bool connected = false;
  if (staConfigured) {
    beginStationAttempt();
    Serial.printf("Подключение к Wi-Fi «%s» (ожидание до %lu секунд)...\n",
                  appConfig.wifiSsid.c_str(),
                  static_cast<unsigned long>(WIFI_BOOT_TIMEOUT_MS / 1000));
    const uint32_t startedAt = millis();
    while (millis() - startedAt < WIFI_BOOT_TIMEOUT_MS) {
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED || status == WL_CONNECT_FAILED) break;
      feedWatchdog();
      serviceSerialConsole();
      delay(250);
    }
    connected = WiFi.status() == WL_CONNECTED;
    if (!connected) {
      Serial.printf("Wi-Fi: %s. Поднимаю точку настройки, подключение продолжится в фоне.\n",
                    wifiStatusText(WiFi.status()));
    }
  } else {
    Serial.println("Wi-Fi не настроен.");
  }

  if (connected) {
    onStationConnected();
  } else {
    staLostSinceMs = millis();
    startAccessPoint();
  }
  webConfigServer.begin(configStore, appConfig);
}

// Вызывается из loop(): переподключение STA, точка настройки, DNS, OTA.
static void serviceWifi() {
  if (apActive) dnsServer.processNextRequest();

  const uint32_t now = millis();
  if (now - lastWifiServiceMs < 500) return;
  lastWifiServiceMs = now;

  if (!staConfigured) {
    if (!apActive) startAccessPoint();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    staAttemptPaused = false;
    if (!staWasConnected) onStationConnected();
    if (apActive && now - staConnectedSinceMs >= AP_IDLE_SHUTDOWN_MS &&
        WiFi.softAPgetStationNum() == 0) {
      stopAccessPoint();
    }
    return;
  }

  if (staWasConnected) {
    staWasConnected = false;
    staLostSinceMs = now;
    Serial.println("Wi-Fi соединение потеряно, переподключение...");
    // Первая попытка — почти сразу.
    lastStaAttemptMs = now - WIFI_RETRY_INTERVAL_MS + 2000;
  }
  if (!apActive && now - staLostSinceMs >= WIFI_LOST_AP_DELAY_MS) startAccessPoint();

  // Пока к точке настройки подключён клиент, не сканируем эфир: попытки STA
  // переключают канал радио и страница настроек «отваливается».
  if (apActive && WiFi.softAPgetStationNum() > 0) {
    if (!staAttemptPaused) {
      WiFi.disconnect(false, false);
      staAttemptPaused = true;
    }
    return;
  }
  staAttemptPaused = false;

  if (now - lastStaAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    Serial.printf("Wi-Fi: %s, повторная попытка подключения к «%s».\n",
                  wifiStatusText(WiFi.status()), appConfig.wifiSsid.c_str());
    beginStationAttempt();
  }
}

// ----------------------------------------------------------------- камера ---

static void tryStartCamera() {
  lastCameraAttemptMs = millis();
  if (systemHealth.isBlocked(MODULE_CAMERA)) {
    cameraStatus = "отключена после аварийного перезапуска";
    return;
  }
  bool ok;
  {
    StageGuard guard(systemHealth, BootStage::Camera);
    ok = cameraService.begin();
  }
  emptyMotionFrames = 0;
  motionDetector.reset();
  if (ok) {
    cameraStatus = "работает";
    Serial.println("Камера готова.");
  } else {
    cameraStatus = "ошибка (" + cameraService.lastError() + "), повтор каждые " +
                   String(CAMERA_RETRY_INTERVAL_MS / 60000) + " мин";
    Serial.println("Камера недоступна: " + cameraService.lastError() +
                   ". Остальные функции продолжают работу.");
  }
}

static void markCameraFailed(const String &reason) {
  cameraService.end();
  lastCameraAttemptMs = millis();
  cameraStatus = "сбой (" + reason + "), повтор через " +
                 String(CAMERA_RETRY_INTERVAL_MS / 60000) + " мин";
  Serial.println("Камера: " + cameraStatus);
}

static bool switchCameraToMotion() {
  bool ok;
  {
    StageGuard guard(systemHealth, BootStage::Camera);
    ok = cameraService.enterMotionMode();
  }
  motionDetector.reset();
  emptyMotionFrames = 0;
  if (!ok) markCameraFailed(cameraService.lastError());
  return ok;
}

static bool switchCameraToPhoto() {
  bool ok;
  {
    StageGuard guard(systemHealth, BootStage::Camera);
    ok = cameraService.enterPhotoMode();
  }
  if (!ok) {
    Serial.println("Камера: не удалось перейти в режим фото: " + cameraService.lastError());
    switchCameraToMotion();
  }
  return ok;
}

// --------------------------------------------------------------- Telegram ---

static void queuePhotos(PhotoKind kind, uint32_t count) {
  if (kind == PhotoKind::Motion) pendingMotionPhotos += count;
  else if (kind == PhotoKind::Manual) pendingManualPhotos += count;
  else pendingPeriodicPhotos += count;
}

static bool canSendNow() {
  return telegramEnabled && WiFi.status() == WL_CONNECTED &&
         !scheduleManager.isQuietNow();
}

static void notifyOrQueue(const String &text, bool ownerArrival) {
  if (!telegramEnabled) return;
  if (canSendNow() && telegramService.sendMessage(text)) return;
  if (ownerArrival) ++pendingOwnerArrivals;
  else ++pendingOwnerDepartures;
}

static void sendPhotoBurst(uint8_t count, const String &caption, PhotoKind kind) {
  if (!telegramEnabled) {
    if (!telegramConfigurationWarningPrinted) {
      Serial.println("Съёмка/отправка пропущена: Telegram не настроен или отключён.");
      telegramConfigurationWarningPrinted = true;
    }
    return;
  }
  if (!cameraService.isReady()) {
    if (kind == PhotoKind::Manual && canSendNow()) {
      telegramService.sendMessage("Камера недоступна: " + cameraStatus);
    }
    return;
  }
  if (!canSendNow()) {
    queuePhotos(kind, count);
    return;
  }
  if (!switchCameraToPhoto()) {
    queuePhotos(kind, count);
    return;
  }

  for (uint8_t i = 0; i < count; ++i) {
    feedWatchdog();
    if (!canSendNow()) {
      queuePhotos(kind, count - i);
      break;
    }
    camera_fb_t *frame = cameraService.capturePhoto();
    if (frame == nullptr) {
      Serial.println("Камера: " + cameraService.lastError());
      queuePhotos(kind, count - i);
      break;
    }
    String photoCaption = caption;
    if (count > 1) photoCaption += " (" + String(i + 1) + "/" + String(count) + ")";
    const bool sent = telegramService.sendPhoto(frame, photoCaption);
    cameraService.releaseFrame(frame);
    if (!sent) {
      Serial.println("Telegram: " + telegramService.lastResult());
      queuePhotos(kind, count - i);
      break;
    }
    if (i + 1 < count) delay(700);
  }
  switchCameraToMotion();
}

static void flushPendingWork() {
  if (!canSendNow()) return;

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
  const bool attachPhoto = hasPhotos && cameraService.isReady();
  if (attachPhoto) {
    summary += "\nБез SD-карты изображения не сохранялись; ниже будет отправлен один актуальный кадр.";
  } else if (hasPhotos) {
    summary += "\nКамера сейчас недоступна: " + cameraStatus;
  }

  if (!telegramService.sendMessage(summary)) return;
  // Сводка доставлена — счётчики обнуляем, чтобы не присылать её повторно.
  pendingOwnerArrivals = 0;
  pendingOwnerDepartures = 0;
  pendingMotionPhotos = 0;
  pendingManualPhotos = 0;
  pendingPeriodicPhotos = 0;

  if (attachPhoto && switchCameraToPhoto()) {
    camera_fb_t *frame = cameraService.capturePhoto();
    if (frame != nullptr) {
      telegramService.sendPhoto(frame, "Актуальный кадр");
      cameraService.releaseFrame(frame);
    }
    switchCameraToMotion();
  }
}

static void initTelegram() {
  if (systemHealth.isBlocked(MODULE_TELEGRAM)) {
    telegramEnabled = false;
    telegramStatus = "отключён (безопасный режим)";
    Serial.println("Telegram: " + telegramStatus);
    return;
  }
  telegramService.configure(appConfig.telegramToken, appConfig.chatId);
  telegramEnabled = telegramService.isConfigured();
  if (telegramEnabled) {
    telegramStatus = "настроен";
  } else {
    telegramStatus = "отключён: " + telegramService.configurationProblem();
    Serial.println("Telegram " + telegramStatus);
  }
}

static void initBluetooth() {
  if (!bleWanted) {
    bluetoothPresence.setStatus(systemHealth.isBlocked(MODULE_BLE)
                                    ? "отключён после аварийного перезапуска"
                                    : "выключен (MAC не задан)");
    Serial.println("BLE: " + bluetoothPresence.status());
    return;
  }
  StageGuard guard(systemHealth, BootStage::Bluetooth);
  bluetoothPresence.begin(appConfig.ownerMac);
}

// ---------------------------------------------------------- веб-состояние ---

static String formatUptime(uint32_t ms) {
  uint32_t seconds = ms / 1000;
  const uint32_t days = seconds / 86400;
  seconds %= 86400;
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lu д %02lu:%02lu:%02lu", static_cast<unsigned long>(days),
           static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds % 3600) / 60),
           static_cast<unsigned long>(seconds % 60));
  return buffer;
}

// Строка, начинающаяся с '!', подсвечивается на странице как проблема.
static String buildStatus() {
  String s;
  s += "Прошивка " + String(FIRMWARE_VERSION) + ", работает " + formatUptime(millis()) +
       ", причина запуска: " + systemHealth.resetReasonText() + "\n";
  if (systemHealth.bootNote().length()) s += "!" + systemHealth.bootNote() + "\n";
  if (systemHealth.safeMode()) s += "!Безопасный режим: камера, BLE и Telegram отключены.\n";
  if (!configStore.ready()) s += "!NVS недоступна: настройки не сохраняются.\n";
  if (!devicePasswordSaved) s += "!Пароль устройства не сохранён в NVS.\n";

  if (!staConfigured) {
    s += "!Wi-Fi: сеть не задана\n";
  } else if (WiFi.status() == WL_CONNECTED) {
    s += "Wi-Fi: «" + appConfig.wifiSsid + "», IP " + WiFi.localIP().toString() +
         ", RSSI " + String(WiFi.RSSI()) + " дБм\n";
  } else {
    s += "!Wi-Fi: «" + appConfig.wifiSsid + "» — " + wifiStatusText(WiFi.status()) +
         (staAttemptPaused ? ", попытки приостановлены, пока открыта точка настройки"
                           : ", повтор каждые 30 с") + "\n";
  }
  if (apActive) {
    s += "Точка настройки: " + apName + ", " + WiFi.softAPIP().toString() + ", клиентов " +
         String(WiFi.softAPgetStationNum()) + "\n";
  }

  s += String(cameraService.isReady() ? "" : "!") + "Камера: " + cameraStatus + "\n";

  const bool bleProblem = bleWanted && !bluetoothPresence.isEnabled();
  s += String(bleProblem ? "!" : "") + "BLE: " + bluetoothPresence.status();
  if (bluetoothPresence.isEnabled()) {
    s += bluetoothPresence.isPresent() ? ", владелец рядом" : ", владелец не обнаружен";
  }
  s += "\n";

  s += String(telegramEnabled ? "" : "!") + "Telegram: " + telegramStatus;
  if (telegramEnabled) s += "; " + telegramService.lastResult();
  s += "\n";

  s += "OTA: " + String(otaStarted ? "активна" : (appConfig.otaEnabled ? "ждёт Wi-Fi" : "выключена")) + "\n";
  s += "Тихие часы: " + String(scheduleManager.isQuietNow() ? "сейчас действуют" : "не действуют") +
       (time(nullptr) < 1700000000 ? " (время ещё не синхронизировано)" : "") + "\n";
  s += "Память: внутр. " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
       " Б, PSRAM " + String(ESP.getFreePsram()) + " Б\n";
  const uint32_t pending = pendingMotionPhotos + pendingManualPhotos + pendingPeriodicPhotos +
                           pendingOwnerArrivals + pendingOwnerDepartures;
  if (pending) s += "Отложено событий: " + String(pending) + "\n";
  return s;
}

// --------------------------------------------------------- Serial-консоль ---
// Сервисные команды через Serial Monitor (115200 бод, любой конец строки).
// Доступ к UART = физический доступ к плате, поэтому пароль здесь можно
// посмотреть или сбросить.

String serialLine;
uint32_t lastSerialCharMs = 0;

static void printSerialHelp() {
  Serial.println("Команды Serial-консоли:");
  Serial.println("  password              — показать пароль устройства (admin, AP, OTA)");
  Serial.println("  resetpassword         — вернуть пароль по умолчанию (Guard + 5 цифр из MAC)");
  Serial.println("  setpassword <пароль>  — задать свой пароль (8–63 символа)");
  Serial.println("  config                — показать настройки (без секретов)");
  Serial.println("  status                — состояние модулей");
  Serial.println("  reboot                — перезапуск");
  Serial.println("  factoryreset yes      — стереть все настройки и перезапуститься");
  Serial.println("  about                 — что это за устройство и как им пользоваться");
}

static void printAbout() {
  Serial.println();
  Serial.println("=============== AI-Thinker Guard ===============");
  Serial.printf("Прошивка %s. Плата AI-Thinker ESP32-CAM (OV2640), Arduino-ESP32 2.0.17.\n", FIRMWARE_VERSION);
  Serial.println("Исходники: https://github.com/KNikolaich/AI-Thinker-Guard");
  Serial.println("           (локально: F:\\Docs\\Git\\AIThinkerGuard)");
  Serial.println();
  Serial.println("ЧТО ДЕЛАЕТ:");
  Serial.println(" Камера-сторож с уведомлениями в Telegram.");
  Serial.println(" - Сравнивает кадры и при движении шлёт N фото в Telegram (не чаще раза в TO секунд).");
  Serial.println(" - Раз в X минут шлёт плановый снимок.");
  Serial.println(" - На команду боту GetCapture (или /GetCapture) шлёт M фото.");
  Serial.println(" - Если задан BLE MAC телефона: пока телефон рядом — тревоги не шлёт;");
  Serial.println("   сообщает «Капитан на постике» (пришёл) и «Сторож бдит» (ушёл).");
  Serial.println(" - Тихие часы: события копятся и приходят сводкой после окончания.");
  Serial.println();
  Serial.println("КАК НАСТРОИТЬ:");
  Serial.println(" 1. Если Wi-Fi не задан или недоступен — устройство поднимает точку Guard-XXXXXX.");
  Serial.println("    Пароль точки — команда password. Через точку панель открывается без логина,");
  Serial.println("    из домашней сети — логин admin и тот же пароль.");
  Serial.println("    По умолчанию это Guard + 5 цифр из MAC; печатается и при каждой загрузке.");
  Serial.println(" 2. Подключиться к точке со смартфона, открыть http://192.168.4.1/");
  Serial.println(" 3. Указать домашний Wi-Fi, токен бота (@BotFather) и свой chat ID, сохранить.");
  Serial.println(" 4. После подключения к дому панель доступна по IP устройства (см. status).");
  Serial.println(" 5. Написать боту /start, затем GetCapture — должно прийти фото.");
  Serial.println();
  Serial.println("ПОЛЕЗНОЕ:");
  Serial.println(" - status — что работает, а что нет; config — текущие настройки.");
  Serial.println(" - Модуль, вызвавший сбой (камера/BLE), отключается до перезапуска по питанию.");
  Serial.println(" - Прошивка: плата \"ESP32 Dev Module\" или \"AI Thinker ESP32-CAM\", PSRAM Enabled,");
  Serial.println("   Flash Mode DIO, схема Minimal SPIFFS (1.9MB APP with OTA). Нужна ArduinoJson 6.x.");
  Serial.println(" - Прошивка по USB: GPIO0 на GND при сбросе.");
  Serial.println(" - Обновление по сети (если разрешено в панели): http://<IP>/update — загрузить");
  Serial.println("   AIThinkerGuard.ino.bin (Скетч -> Экспорт скомпилированного бинарного файла).");
  Serial.println("================================================");
}

static void applyNewPassword(const String &password, bool custom) {
  appConfig.devicePassword = password;
  appConfig.devicePasswordCustom = custom;
  devicePasswordSaved = configStore.save(appConfig);
  Serial.printf("Новый пароль устройства: %s\n", password.c_str());
  Serial.println(devicePasswordSaved
                     ? "Сохранён. Точка доступа и OTA перейдут на него после перезапуска (reboot); веб-панель — сразу."
                     : "ВНИМАНИЕ: NVS недоступна, пароль действует только до перезапуска.");
}

static void handleSerialCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;
  Serial.println("> " + line);  // эхо: видно, что команда дошла до платы
  const int space = line.indexOf(' ');
  String command = space < 0 ? line : line.substring(0, space);
  String argument = space < 0 ? "" : line.substring(space + 1);
  command.toLowerCase();
  argument.trim();

  if (command == "help" || command == "?") {
    printSerialHelp();
  } else if (command == "password") {
    const String pointName = apName.isEmpty() ? makeAccessPointName() : apName;
    Serial.printf("Пароль устройства (пользователь admin, точка %s, OTA): %s\n",
                  pointName.c_str(), appConfig.devicePassword.c_str());
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Веб-панель: http://" + WiFi.localIP().toString() + "/");
    }
    if (apActive) Serial.println("Веб-панель в точке настройки: http://" + WiFi.softAPIP().toString() + "/");
  } else if (command == "resetpassword") {
    applyNewPassword(defaultDevicePassword(), false);
  } else if (command == "setpassword") {
    if (!isValidDevicePassword(argument)) {
      Serial.println("Пароль: 8–63 символа, только латиница, цифры и знаки (без кириллицы).");
    } else {
      applyNewPassword(argument, true);
    }
  } else if (command == "about") {
    printAbout();
  } else if (command == "config") {
    printConfiguration();
  } else if (command == "status") {
    Serial.print(buildStatus());
  } else if (command == "reboot") {
    Serial.println("Перезапуск...");
    Serial.flush();
    systemHealth.clearBlocks();
    ESP.restart();
  } else if (command == "factoryreset") {
    if (argument != "yes") {
      Serial.println("Для подтверждения введите: factoryreset yes");
      return;
    }
    const bool cleared = configStore.clearAll();
    Serial.println(cleared ? "Настройки стёрты. Перезапуск; новый пароль будет напечатан при старте."
                           : "Не удалось стереть настройки (NVS недоступна).");
    Serial.flush();
    if (cleared) {
      systemHealth.clearBlocks();
      ESP.restart();
    }
  } else {
    Serial.println("Неизвестная команда: " + command);
    printSerialHelp();
  }
}

static void serviceSerialConsole() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    lastSerialCharMs = millis();
    if (c == '\r' || c == '\n') {
      handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 96) {
      serialLine += c;
    }
  }
  // Serial Monitor в режиме «Нет конца строки»: выполняем после паузы.
  if (serialLine.length() && millis() - lastSerialCharMs > 400) {
    handleSerialCommand(serialLine);
    serialLine = "";
  }
}

static String maskedText(const String &value) {
  if (value.isEmpty()) return "(не задан)";
  return "задан (" + String(value.length()) + " симв.)";
}

// Пароль устройства печатается при каждой загрузке (по просьбе владельца);
// пароль Wi-Fi и токен Telegram — только признаком «задан».
static void printConfiguration() {
  Serial.println("---------------- Настройки ----------------");
  Serial.println("NVS: " + String(configStore.ready() ? "доступна" : "НЕДОСТУПНА (значения по умолчанию)"));
  Serial.println("Пароль устройства (логин admin, точка доступа, OTA): " +
                 (appConfig.devicePasswordCustom ? appConfig.devicePassword + " (задан вручную)"
                                                 : defaultDevicePassword() + " (по умолчанию, из MAC)"));
  Serial.println("Wi-Fi SSID: " + (appConfig.wifiSsid.isEmpty() ? String("(не задан)") : "«" + appConfig.wifiSsid + "»"));
  Serial.println("Wi-Fi пароль: " + maskedText(appConfig.wifiPassword));
  Serial.println("Telegram токен: " + maskedText(appConfig.telegramToken));
  Serial.println("Telegram chat ID: " + (appConfig.chatId.isEmpty() ? String("(не задан)") : appConfig.chatId));
  Serial.println("BLE MAC владельца: " + (appConfig.ownerMac.isEmpty() ? String("(не задан, BLE выключен)") : appConfig.ownerMac));
  Serial.printf("N (фото по движению): %u, M (GetCapture): %u\n",
                static_cast<unsigned>(appConfig.motionCount), static_cast<unsigned>(appConfig.manualCount));
  Serial.printf("X (плановый снимок): %u мин, TO (пауза тревог): %lu с\n",
                static_cast<unsigned>(appConfig.periodicMinutes),
                static_cast<unsigned long>(appConfig.timeoutSeconds));
  Serial.printf("Тихие часы: %s, %s–%s, UTC%+d мин\n", appConfig.quietEnabled ? "вкл" : "выкл",
                appConfig.quietStart.c_str(), appConfig.quietEnd.c_str(),
                static_cast<int>(appConfig.timezoneOffsetMinutes));
  Serial.println("OTA: " + String(appConfig.otaEnabled ? "разрешена" : "выключена"));
  Serial.println("Команды Serial: help (115200 бод)");
  Serial.println("-------------------------------------------");
}

// ------------------------------------------------------------ setup/loop ---

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nAI-Thinker Guard firmware %s\n", FIRMWARE_VERSION);

  systemHealth.begin();
  Serial.println("Причина запуска: " + systemHealth.resetReasonText());
  if (systemHealth.bootNote().length()) Serial.println(systemHealth.bootNote());

  // Сторожевой таймер основного цикла: зависание -> перезапуск, а не «кирпич».
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);

  Serial.println("[init] NVS...");
  systemHealth.enterStage(BootStage::Storage);
  if (!configStore.begin()) {
    Serial.println("ОШИБКА: NVS недоступна. Работаю с настройками по умолчанию.");
  }
  configStore.load(appConfig);
  telegramUpdateOffset = configStore.loadUpdateOffset();
  printConfiguration();

  bleWanted = BluetoothPresence::isValidMac(appConfig.ownerMac) &&
              !systemHealth.isBlocked(MODULE_BLE);

  Serial.println("[init] Сеть и веб-конфигурация...");
  systemHealth.enterStage(BootStage::Network);
  startNetworkAndPortal();
  feedWatchdog();

  Serial.println("[init] Камера...");
  tryStartCamera();
  feedWatchdog();

  Serial.println("[init] Telegram...");
  initTelegram();

  Serial.println("[init] NTP/расписание...");
  scheduleManager.begin(appConfig);

  Serial.println("[init] BLE...");
  initBluetooth();
  feedWatchdog();

  webConfigServer.setStatusProvider(buildStatus);
  webConfigServer.setFirmwareVersion(FIRMWARE_VERSION);
  webConfigServer.setBeforeRestart([]() { systemHealth.clearBlocks(); });

  previousOwnerPresent = bluetoothPresence.isPresent();
  ownerStateInitialized = true;
  wasQuiet = scheduleManager.isQuietNow();
  lastPeriodicCaptureMs = millis();
  lastPendingFlushMs = millis() - 60000;

  systemHealth.enterStage(BootStage::Running);
  Serial.println("Инициализация завершена. Состояние:");
  Serial.print(buildStatus());
  Serial.println("Команды Serial Monitor: help; что это за устройство — about.");
}

void loop() {
  feedWatchdog();
  systemHealth.loop();
  serviceSerialConsole();
  serviceWifi();
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
    notifyOrQueue(ownerPresent ? "Капитан на постике" : "Сторож бдит", ownerPresent);
  }

  if (!cameraService.isReady() && !systemHealth.isBlocked(MODULE_CAMERA) &&
      now - lastCameraAttemptMs >= CAMERA_RETRY_INTERVAL_MS) {
    Serial.println("Повторная инициализация камеры...");
    tryStartCamera();
  }

  if (telegramEnabled && WiFi.status() == WL_CONNECTED &&
      now - lastTelegramPollMs >= telegramPollIntervalMs) {
    lastTelegramPollMs = now;
    String command;
    const int64_t oldOffset = telegramUpdateOffset;
    const bool gotCommand = telegramService.pollCommand(telegramUpdateOffset, command);
    // Без интернета каждый запрос блокирует цикл до таймаута TLS, поэтому
    // интервал опроса растёт до минуты и сбрасывается после успеха.
    if (telegramService.lastRequestOk()) {
      telegramPollIntervalMs = TELEGRAM_POLL_INTERVAL_MS;
    } else {
      telegramPollIntervalMs = telegramPollIntervalMs * 2 > TELEGRAM_MAX_BACKOFF_MS
                                   ? TELEGRAM_MAX_BACKOFF_MS
                                   : telegramPollIntervalMs * 2;
    }
    if (gotCommand && command == "getcapture") {
      sendPhotoBurst(appConfig.manualCount, "Снимок по запросу GetCapture", PhotoKind::Manual);
    }
    if (telegramUpdateOffset != oldOffset) configStore.saveUpdateOffset(telegramUpdateOffset);
  }

  if (cameraService.isReady() && telegramEnabled &&
      now - lastMotionSampleMs >= MOTION_SAMPLE_INTERVAL_MS) {
    lastMotionSampleMs = now;
    camera_fb_t *frame = cameraService.captureMotionFrame();
    if (frame == nullptr) {
      if (++emptyMotionFrames >= CAMERA_MAX_EMPTY_FRAMES) {
        markCameraFailed("нет кадров");
      }
    } else {
      emptyMotionFrames = 0;
      const bool movementDetected = motionDetector.detect(frame->buf, frame->len);
      cameraService.releaseFrame(frame);
      const uint32_t timeoutMs = static_cast<uint32_t>(appConfig.timeoutSeconds) * 1000UL;
      if (movementDetected && !ownerPresent &&
          (!motionActionStarted || now - lastMotionActionMs >= timeoutMs)) {
        motionActionStarted = true;
        lastMotionActionMs = now;
        sendPhotoBurst(appConfig.motionCount, "Обнаружено движение", PhotoKind::Motion);
      }
    }
  }

  const uint32_t periodicMs = static_cast<uint32_t>(appConfig.periodicMinutes) * 60000UL;
  if (cameraService.isReady() && telegramEnabled &&
      millis() - lastPeriodicCaptureMs >= periodicMs) {
    lastPeriodicCaptureMs = millis();
    sendPhotoBurst(1, "Плановый снимок", PhotoKind::Periodic);
  }

  flushPendingWork();
  delay(5);
}
