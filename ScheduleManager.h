#pragma once

#include <Arduino.h>
#include "Config.h"

class ScheduleManager {
 public:
  void begin(const AppConfig &config);
  bool isQuietNow() const;

 private:
  static bool parseTime(const String &value, int &minutes);
  bool enabled_ = false;
  String quietStart_;
  String quietEnd_;
};