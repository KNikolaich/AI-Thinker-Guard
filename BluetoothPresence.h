#pragma once

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>

class BluetoothPresence : public BLEAdvertisedDeviceCallbacks {
 public:
  // Возвращает true, если сканирование запущено. Никогда не останавливает
  // остальную прошивку: при ошибке BLE просто остаётся выключенным.
  bool begin(const String &targetMac);
  bool isEnabled() const;
  bool isPresent() const;
  const String &status() const { return status_; }
  void setStatus(const String &status) { status_ = status; }
  void onResult(BLEAdvertisedDevice advertisedDevice) override;

  static bool isValidMac(const String &mac);

 private:
  static void scanTask(void *parameter);
  static String normalizeMac(String mac);
  String targetMac_;
  String status_ = "не инициализирован";
  BLEScan *scan_ = nullptr;
  volatile uint32_t lastSeenMs_ = 0;
  volatile bool enabled_ = false;
};
