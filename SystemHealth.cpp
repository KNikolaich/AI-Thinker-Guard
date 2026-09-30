#include "SystemHealth.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <string.h>

namespace {

const uint32_t HEALTH_MAGIC = 0x47554152;  // "GUAR"
const uint8_t SAFE_MODE_STREAK = 3;
const uint32_t STABLE_UPTIME_MS = 180000;

struct RtcHealth {
  uint32_t magic;
  uint8_t stage;
  uint8_t streak;
  uint8_t blocked;
  uint8_t safe;
  uint32_t check;
};

// RTC_NOINIT: не обнуляется при программном перезапуске, panic и WDT,
// но теряется при отключении питания — это и есть «полный сброс».
RTC_NOINIT_ATTR RtcHealth rtcHealth;

uint32_t checksum(const RtcHealth &h) {
  const uint32_t packed = static_cast<uint32_t>(h.stage) |
                          (static_cast<uint32_t>(h.streak) << 8) |
                          (static_cast<uint32_t>(h.blocked) << 16) |
                          (static_cast<uint32_t>(h.safe) << 24);
  return (h.magic ^ packed) ^ 0xA5A5F00Du;
}

void commit() {
  rtcHealth.check = checksum(rtcHealth);
}

bool isValid() {
  return rtcHealth.magic == HEALTH_MAGIC && rtcHealth.check == checksum(rtcHealth);
}

void resetState() {
  memset(&rtcHealth, 0, sizeof(rtcHealth));
  rtcHealth.magic = HEALTH_MAGIC;
  commit();
}

bool isCrashReset(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

}  // namespace

const char *SystemHealth::stageName(BootStage stage) {
  switch (stage) {
    case BootStage::None: return "нет";
    case BootStage::Storage: return "NVS";
    case BootStage::Network: return "Wi-Fi/веб";
    case BootStage::Camera: return "камера";
    case BootStage::Running: return "основной цикл";
  }
  return "?";
}

void SystemHealth::begin() {
  const esp_reset_reason_t reason = esp_reset_reason();
  resetReason_ = static_cast<int>(reason);

  if (!isValid() || reason == ESP_RST_POWERON || reason == ESP_RST_EXT) {
    resetState();
  }

  if (isCrashReset(reason)) {
    const BootStage failed = static_cast<BootStage>(rtcHealth.stage);
    bootNote_ = "Предыдущий запуск завершился сбоем (" + resetReasonText() +
                ") на этапе «" + String(stageName(failed)) + "». ";
    if (failed == BootStage::Camera) {
      rtcHealth.blocked |= MODULE_CAMERA;
      bootNote_ += "Камера отключена до перезапуска по питанию, сохранения настроек или кнопки «Повторить».";
    } else {
      if (rtcHealth.streak < 255) ++rtcHealth.streak;
      bootNote_ += "Сбоев подряд: " + String(rtcHealth.streak) + ".";
      if (rtcHealth.streak >= SAFE_MODE_STREAK) {
        rtcHealth.safe = 1;
        rtcHealth.blocked |= MODULE_CAMERA | MODULE_TELEGRAM;
        bootNote_ += " Включён безопасный режим: работают только Wi-Fi и страница настроек.";
      }
    }
    if (reason == ESP_RST_BROWNOUT) {
      bootNote_ += " Сработала защита от просадки питания: нужен стабильный источник 5 В / 2 А и короткие провода.";
    }
  }

  rtcHealth.stage = static_cast<uint8_t>(BootStage::None);
  commit();
}

void SystemHealth::enterStage(BootStage stage) {
  rtcHealth.stage = static_cast<uint8_t>(stage);
  commit();
}

BootStage SystemHealth::stage() const {
  return static_cast<BootStage>(rtcHealth.stage);
}

bool SystemHealth::isBlocked(uint8_t module) const {
  return (rtcHealth.blocked & module) != 0;
}

bool SystemHealth::safeMode() const {
  return rtcHealth.safe != 0;
}

uint8_t SystemHealth::crashStreak() const {
  return rtcHealth.streak;
}

void SystemHealth::loop() {
  if (stableMarked_ || millis() < STABLE_UPTIME_MS) return;
  stableMarked_ = true;
  if (rtcHealth.streak != 0) {
    rtcHealth.streak = 0;
    commit();
  }
}

void SystemHealth::clearBlocks() {
  rtcHealth.blocked = 0;
  rtcHealth.safe = 0;
  rtcHealth.streak = 0;
  rtcHealth.stage = static_cast<uint8_t>(BootStage::None);
  commit();
}

String SystemHealth::resetReasonText() const {
  switch (static_cast<esp_reset_reason_t>(resetReason_)) {
    case ESP_RST_POWERON: return "включение питания";
    case ESP_RST_EXT: return "внешний сброс";
    case ESP_RST_SW: return "программный перезапуск";
    case ESP_RST_PANIC: return "исключение/abort";
    case ESP_RST_INT_WDT: return "watchdog прерываний";
    case ESP_RST_TASK_WDT: return "watchdog задачи";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "выход из deep sleep";
    case ESP_RST_BROWNOUT: return "просадка питания";
    case ESP_RST_SDIO: return "SDIO";
    default: return "неизвестно";
  }
}
