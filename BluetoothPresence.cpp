#include "BluetoothPresence.h"

String BluetoothPresence::normalizeMac(String mac) {
  mac.toLowerCase();
  String normalized;
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    if (isxdigit(static_cast<unsigned char>(c))) normalized += c;
  }
  return normalized;
}

void BluetoothPresence::begin(const String &targetMac) {
  targetMac_ = normalizeMac(targetMac);
  if (targetMac_.length() != 12) {
    Serial.println("BLE-контроль отключён: MAC не задан или имеет неверный формат.");
    return;
  }

  BLEDevice::init("");
  scan_ = BLEDevice::getScan();
  if (scan_ == nullptr) return;
  scan_->setAdvertisedDeviceCallbacks(this, true);
  scan_->setActiveScan(true);
  scan_->setInterval(100);
  scan_->setWindow(80);
  enabled_ = true;
  const BaseType_t result = xTaskCreatePinnedToCore(
      scanTask, "blePresence", 4096, this, 1, nullptr, 0);
  if (result != pdPASS) {
    enabled_ = false;
    Serial.println("Не удалось запустить задачу BLE-сканирования.");
    return;
  }
  Serial.println("BLE-сканирование MAC владельца включено.");
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
  while (self->enabled_) {
    self->scan_->start(2, false);
    self->scan_->clearResults();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  vTaskDelete(nullptr);
}