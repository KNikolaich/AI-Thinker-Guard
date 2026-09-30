#include "Config.h"

#include <nvs_flash.h>

static bool putStringAndVerify(Preferences &preferences, const char *key,
                               const String &value) {
  preferences.putString(key, value);
  return preferences.isKey(key) && preferences.getString(key, "\x01") == value;
}

bool ConfigStore::begin() {
  ready_ = preferences_.begin("guard", false);
  if (ready_) return true;

  // Раздел NVS повреждён или не инициализирован: пробуем восстановить его.
  // Настройки при этом теряются, но устройство остаётся управляемым.
  Serial.println("NVS: не удалось открыть раздел, выполняю очистку и повторную инициализацию...");
  nvs_flash_deinit();
  const esp_err_t eraseResult = nvs_flash_erase();
  const esp_err_t initResult = nvs_flash_init();
  Serial.printf("NVS: erase=0x%x init=0x%x\n", static_cast<unsigned>(eraseResult),
                static_cast<unsigned>(initResult));
  ready_ = preferences_.begin("guard", false);
  return ready_;
}

void ConfigStore::load(AppConfig &config) {
  config.wifiSsid = preferences_.getString("wifiSsid", "");
  config.wifiPassword = preferences_.getString("wifiPass", "");
  config.telegramToken = preferences_.getString("tgToken", "");
  config.chatId = preferences_.getString("chatId", "");
  config.ownerMac = preferences_.getString("ownerMac", "");
  if (preferences_.isKey("devicePass")) {
    config.devicePassword = preferences_.getString("devicePass", "");
  } else {
    // Migrate the old web password, but never silently enable OTA on upgrade.
    config.devicePassword = preferences_.getString("webPass", "");
    if (putStringAndVerify(preferences_, "devicePass", config.devicePassword)) {
      preferences_.remove("webPass");
      preferences_.remove("otaPass");
    }
  }
  config.devicePasswordCustom = preferences_.getBool("passCustom", false);
  config.motionCount = preferences_.getUChar("motionN", 3);
  config.manualCount = preferences_.getUChar("manualM", 3);
  config.periodicMinutes = preferences_.getUShort("periodX", 30);
  config.timeoutSeconds = preferences_.getUInt("timeout", 300);
  config.otaEnabled = preferences_.getBool("otaEnabled", false);
  config.quietEnabled = preferences_.getBool("quietOn", false);
  config.quietStart = preferences_.getString("quietStart", "23:00");
  config.quietEnd = preferences_.getString("quietEnd", "07:00");
  config.timezoneOffsetMinutes = preferences_.getShort("tzOffset", 180);
}

bool ConfigStore::save(const AppConfig &config) {
  if (!ready_) return false;
  bool ok = true;
  ok &= putStringAndVerify(preferences_, "wifiSsid", config.wifiSsid);
  ok &= putStringAndVerify(preferences_, "wifiPass", config.wifiPassword);
  ok &= putStringAndVerify(preferences_, "tgToken", config.telegramToken);
  ok &= putStringAndVerify(preferences_, "chatId", config.chatId);
  ok &= putStringAndVerify(preferences_, "ownerMac", config.ownerMac);
  ok &= putStringAndVerify(preferences_, "devicePass", config.devicePassword);
  ok &= preferences_.putBool("passCustom", config.devicePasswordCustom) > 0;
  ok &= preferences_.putBool("otaEnabled", config.otaEnabled) > 0;
  ok &= preferences_.putUChar("motionN", config.motionCount) > 0;
  ok &= preferences_.putUChar("manualM", config.manualCount) > 0;
  ok &= preferences_.putUShort("periodX", config.periodicMinutes) > 0;
  ok &= preferences_.putUInt("timeout", config.timeoutSeconds) > 0;
  ok &= preferences_.putBool("quietOn", config.quietEnabled) > 0;
  ok &= putStringAndVerify(preferences_, "quietStart", config.quietStart);
  ok &= putStringAndVerify(preferences_, "quietEnd", config.quietEnd);
  ok &= preferences_.putShort("tzOffset", config.timezoneOffsetMinutes) > 0;
  return ok;
}

bool ConfigStore::clearAll() {
  if (!ready_) return false;
  return preferences_.clear();
}

int64_t ConfigStore::loadUpdateOffset() {
  return preferences_.getLong64("tgOffset", 0);
}

void ConfigStore::saveUpdateOffset(int64_t offset) {
  if (!ready_) return;
  preferences_.putLong64("tgOffset", offset);
}