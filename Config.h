#pragma once

#include <Arduino.h>
#include <Preferences.h>

struct AppConfig {
  String wifiSsid;
  String wifiPassword;
  String telegramToken;
  String chatId;
  // Адрес API Telegram: host[:port][/префикс]. Пусто — api.telegram.org.
  // Нужен для работы через свой прокси, если Telegram заблокирован.
  String telegramApiHost;
  // IP телефона капитана в домашней Wi-Fi сети (пусто — функция выключена).
  String ownerIp;
  // Сколько минут телефон может молчать, прежде чем «капитан ушёл».
  uint16_t ownerAwayMinutes = 10;
  String devicePassword;
  // false — пароль по умолчанию, вычисляемый из MAC (Guard + 5 цифр).
  bool devicePasswordCustom = false;
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
  bool ready() const { return ready_; }
  void load(AppConfig &config);
  bool save(const AppConfig &config);
  bool clearAll();  // сброс всех настроек в NVS
  int64_t loadUpdateOffset();
  void saveUpdateOffset(int64_t offset);

 private:
  Preferences preferences_;
  bool ready_ = false;
};