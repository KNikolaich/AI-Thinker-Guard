#include "Config.h"

bool ConfigStore::begin() {
  return preferences_.begin("guard", false);
}

void ConfigStore::load(AppConfig &config) {
  config.wifiSsid = preferences_.getString("wifiSsid", "");
  config.wifiPassword = preferences_.getString("wifiPass", "");
  config.telegramToken = preferences_.getString("tgToken", "");
  config.chatId = preferences_.getString("chatId", "");
  config.ownerMac = preferences_.getString("ownerMac", "");
  config.webPassword = preferences_.getString("webPass", "");
  config.otaPassword = preferences_.getString("otaPass", "");
  config.motionCount = preferences_.getUChar("motionN", 3);
  config.manualCount = preferences_.getUChar("manualM", 3);
  config.periodicMinutes = preferences_.getUShort("periodX", 30);
  config.timeoutSeconds = preferences_.getUInt("timeout", 300);
  config.quietEnabled = preferences_.getBool("quietOn", false);
  config.quietStart = preferences_.getString("quietStart", "23:00");
  config.quietEnd = preferences_.getString("quietEnd", "07:00");
  config.timezoneOffsetMinutes = preferences_.getShort("tzOffset", 180);
}

bool ConfigStore::save(const AppConfig &config) {
  size_t written = 0;
  written += preferences_.putString("wifiSsid", config.wifiSsid);
  written += preferences_.putString("wifiPass", config.wifiPassword);
  written += preferences_.putString("tgToken", config.telegramToken);
  written += preferences_.putString("chatId", config.chatId);
  written += preferences_.putString("ownerMac", config.ownerMac);
  written += preferences_.putString("webPass", config.webPassword);
  written += preferences_.putString("otaPass", config.otaPassword);
  written += preferences_.putUChar("motionN", config.motionCount);
  written += preferences_.putUChar("manualM", config.manualCount);
  written += preferences_.putUShort("periodX", config.periodicMinutes);
  written += preferences_.putUInt("timeout", config.timeoutSeconds);
  written += preferences_.putBool("quietOn", config.quietEnabled);
  written += preferences_.putString("quietStart", config.quietStart);
  written += preferences_.putString("quietEnd", config.quietEnd);
  written += preferences_.putShort("tzOffset", config.timezoneOffsetMinutes);
  return written > 0;
}

int64_t ConfigStore::loadUpdateOffset() {
  return preferences_.getLong64("tgOffset", 0);
}

void ConfigStore::saveUpdateOffset(int64_t offset) {
  preferences_.putLong64("tgOffset", offset);
}