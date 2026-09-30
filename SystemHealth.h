#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>

// Этапы запуска/рискованных операций. Текущий этап хранится в RTC-памяти,
// которая переживает panic/WDT-перезапуск, поэтому после сбоя известно,
// какой модуль его вызвал.
enum class BootStage : uint8_t {
  None = 0,
  Storage,
  Network,
  Camera,
  Running,
};

enum ModuleMask : uint8_t {
  MODULE_CAMERA = 0x01,
  MODULE_TELEGRAM = 0x04,
};

class SystemHealth {
 public:
  // Анализирует причину перезапуска и решает, какие модули пропустить.
  void begin();
  void enterStage(BootStage stage);
  BootStage stage() const;
  bool isBlocked(uint8_t module) const;
  bool safeMode() const;
  uint8_t crashStreak() const;
  // Вызывать из loop(): после нескольких минут без сбоев сбрасывает счётчик.
  void loop();
  // Снимает все блокировки (перед намеренным перезапуском из веб-панели).
  void clearBlocks();
  String resetReasonText() const;
  const String &bootNote() const { return bootNote_; }
  static const char *stageName(BootStage stage);

 private:
  String bootNote_;
  int resetReason_ = 0;
  bool stableMarked_ = false;
};

// Временно переводит систему в этап stage и возвращает предыдущий этап
// при выходе из области видимости.
class StageGuard {
 public:
  StageGuard(SystemHealth &health, BootStage stage)
      : health_(health), previous_(health.stage()) {
    health_.enterStage(stage);
  }
  ~StageGuard() { health_.enterStage(previous_); }

 private:
  SystemHealth &health_;
  BootStage previous_;
};

inline void feedWatchdog() {
  esp_task_wdt_reset();
}
