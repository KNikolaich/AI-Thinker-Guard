#pragma once

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>

class BluetoothPresence : public BLEAdvertisedDeviceCallbacks {
 public:
  void begin(const String &targetMac);
  bool isEnabled() const;
  bool isPresent() const;
  void onResult(BLEAdvertisedDevice advertisedDevice) override;

 private:
  static void scanTask(void *parameter);
  static String normalizeMac(String mac);
  String targetMac_;
  BLEScan *scan_ = nullptr;
  volatile uint32_t lastSeenMs_ = 0;
  bool enabled_ = false;
};