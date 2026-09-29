#pragma once

#include <Arduino.h>
#include <Preferences.h>

struct AppConfig {
  String wifiSsid;
  String wifiPassword;
  String telegramToken;
  String chatId;
  String ownerMac;
  String devicePassword;
  uint8_t motionCount = 3;
  uint8_t manualCount = 3;
  uint16_t periodicMinutes = 30;
  uint32_t timeoutSeconds = 300;
  bool otaEnabled = false;
  bool quietEnabled = false;
  String quietStart = "23:00";
  String quietEnd = "07:00";
  int16_t timezoneOffsetMinutes = 180;
};

class ConfigStore {
 public:
  bool begin();
  void load(AppConfig &config);
  bool save(const AppConfig &config);
  int64_t loadUpdateOffset();
  void saveUpdateOffset(int64_t offset);

 private:
  Preferences preferences_;
};