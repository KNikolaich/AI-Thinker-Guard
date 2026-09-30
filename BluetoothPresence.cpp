#include "BluetoothPresence.h"

#include <ctype.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_heap_caps.h>

// Контроллеру BT и стеку Bluedroid нужно порядка 60–90 КБ внутренней RAM.
// Если её меньше, инициализация может закончиться abort() внутри IDF.
static const size_t BLE_MIN_FREE_INTERNAL = 70000;

String BluetoothPresence::normalizeMac(String mac) {
  mac.toLowerCase();
  mac.trim();
  String normalized;
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    if (isxdigit(static_cast<unsigned char>(c))) normalized += c;
    else if (c != ':' && c != '-') return "";
  }
  return normalized;
}

bool BluetoothPresence::isValidMac(const String &mac) {
  const String normalized = normalizeMac(mac);
  return normalized.length() == 12 && normalized != "000000000000" &&
         normalized != "ffffffffffff";
}

bool BluetoothPresence::begin(const String &targetMac) {
  if (!isValidMac(targetMac)) {
    status_ = "выключен (MAC не задан или некорректен)";
    Serial.println("BLE отключён: MAC пустой или некорректный; остальные функции продолжают работу.");
    return false;
  }
  targetMac_ = normalizeMac(targetMac);

  const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("BLE: свободно внутренней RAM %u байт.\n", static_cast<unsigned>(freeInternal));
  if (freeInternal < BLE_MIN_FREE_INTERNAL) {
    status_ = "не запущен: мало внутренней RAM (" + String(freeInternal) + " байт)";
    Serial.println("BLE: " + status_);
    return false;
  }

  Serial.println("BLE: инициализация контроллера...");
  BLEDevice::init("");
  if (!btStarted() || esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    status_ = "ошибка инициализации контроллера/Bluedroid";
    Serial.println("BLE: " + status_ + "; BLE отключён.");
    return false;
  }

  scan_ = BLEDevice::getScan();
  if (scan_ == nullptr) {
    status_ = "контроллер не вернул сканер";
    Serial.println("BLE: " + status_ + "; BLE отключён.");
    return false;
  }
  scan_->setAdvertisedDeviceCallbacks(this, true);
  // Пассивное сканирование с долей эфира ~30%: MAC виден в рекламе и так,
  // а радиоканал остаётся Wi-Fi (он общий у ESP32).
  scan_->setActiveScan(false);
  scan_->setInterval(160);
  scan_->setWindow(48);

  enabled_ = true;
  const BaseType_t result = xTaskCreatePinnedToCore(
      scanTask, "blePresence", 4096, this, 1, nullptr, 0);
  if (result != pdPASS) {
    enabled_ = false;
    status_ = "не удалось создать задачу сканирования";
    Serial.println("BLE: " + status_);
    return false;
  }
  status_ = "сканирование включено";
  Serial.println("BLE-сканирование MAC владельца включено.");
  return true;
}

bool BluetoothPresence::isEnabled() const {
  return enabled_;
}

bool BluetoothPresence::isPresent() const {
  if (!enabled_) return false;
  const uint32_t seenAt = lastSeenMs_;
  return seenAt != 0 && millis() - seenAt < 12000;
}

void BluetoothPresence::onResult(BLEAdvertisedDevice advertisedDevice) {
  if (normalizeMac(String(advertisedDevice.getAddress().toString().c_str())) == targetMac_) {
    lastSeenMs_ = millis();
  }
}

void BluetoothPresence::scanTask(void *parameter) {
  BluetoothPresence *self = static_cast<BluetoothPresence *>(parameter);
  uint8_t failures = 0;
  while (self->enabled_) {
    // Неблокирующий вариант start() сообщает об ошибке запуска; ждём сами.
    if (self->scan_->start(2, nullptr, false)) {
      failures = 0;
      vTaskDelay(pdMS_TO_TICKS(2100));
    } else if (++failures >= 10) {
      failures = 0;
      Serial.println("BLE: сканирование не запускается, пауза 30 с.");
      vTaskDelay(pdMS_TO_TICKS(30000));
    }
    self->scan_->clearResults();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  vTaskDelete(nullptr);
}
