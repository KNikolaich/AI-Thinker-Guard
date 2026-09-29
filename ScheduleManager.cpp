#include "ScheduleManager.h"

#include <time.h>

void ScheduleManager::begin(const AppConfig &config) {
  enabled_ = config.quietEnabled;
  quietStart_ = config.quietStart;
  quietEnd_ = config.quietEnd;
  const long offsetSeconds = static_cast<long>(config.timezoneOffsetMinutes) * 60L;
  configTime(offsetSeconds, 0, "pool.ntp.org", "time.nist.gov");
}

bool ScheduleManager::parseTime(const String &value, int &minutes) {
  if (value.length() != 5 || value[2] != ':') return false;
  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
  minutes = hour * 60 + minute;
  return true;
}

bool ScheduleManager::isQuietNow() const {
  if (!enabled_) return false;

  int startMinute = 0;
  int endMinute = 0;
  if (!parseTime(quietStart_, startMinute) || !parseTime(quietEnd_, endMinute) ||
      startMinute == endMinute) return false;

  const time_t now = time(nullptr);
  if (now < 1700000000) return false;  // NTP has not synchronized yet.
  struct tm localTime;
  if (localtime_r(&now, &localTime) == nullptr) return false;
  const int currentMinute = localTime.tm_hour * 60 + localTime.tm_min;

  if (startMinute < endMinute) {
    return currentMinute >= startMinute && currentMinute < endMinute;
  }
  return currentMinute >= startMinute || currentMinute < endMinute;
}