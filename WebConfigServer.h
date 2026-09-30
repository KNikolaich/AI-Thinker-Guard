#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <functional>
#include "Config.h"

bool isValidDevicePassword(const String &value);

class WebConfigServer {
 public:
  void begin(ConfigStore &store, AppConfig &config);
  void handleClient();
  // true — точка доступа защищена паролем устройства (WPA2), и её клиентам
  // повторный логин в веб-панели не нужен.
  void setAccessPointProtected(bool value) { apProtected_ = value; }
  // Строки состояния модулей (по одной на строку, без HTML).
  void setStatusProvider(std::function<String()> provider) { statusProvider_ = provider; }
  // Вызывается прямо перед намеренным перезапуском из веб-панели.
  void setBeforeRestart(std::function<void()> callback) { beforeRestart_ = callback; }
  void setFirmwareVersion(const char *version) { firmwareVersion_ = version; }

 private:
  void handleRoot();
  void handleSave();
  void handleRetry();
  void handleUpdatePage();
  void handleUpdateUpload();
  void handleUpdateDone();
  bool updateAllowed();
  void restartSoon(const String &message);
  String statusHtml() const;
  bool authenticate();
  bool requestFromAccessPoint();
  String escapeHtml(const String &value) const;
  String textField(const String &label, const String &name, const String &value,
                   const String &type = "text") const;
  String passwordField(const String &label, const String &name) const;
  WebServer server_{80};
  ConfigStore *store_ = nullptr;
  AppConfig *config_ = nullptr;
  bool apProtected_ = false;
  const char *firmwareVersion_ = "";
  String uploadError_;
  bool uploadStarted_ = false;
  std::function<String()> statusProvider_;
  std::function<void()> beforeRestart_;
};