#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "Config.h"

class WebConfigServer {
 public:
  void begin(ConfigStore &store, AppConfig &config, const String &fallbackPassword);
  void handleClient();

 private:
  void handleRoot();
  void handleSave();
  bool authenticate();
  String escapeHtml(const String &value) const;
  String textField(const String &label, const String &name, const String &value,
                   const String &type = "text") const;
  String passwordField(const String &label, const String &name) const;
  WebServer server_{80};
  ConfigStore *store_ = nullptr;
  AppConfig *config_ = nullptr;
  String fallbackPassword_;
};