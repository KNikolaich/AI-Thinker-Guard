#pragma once

#include <Arduino.h>

class MotionDetector {
 public:
  bool detect(const uint8_t *pixels, size_t length);
  void reset();

 private:
  static const size_t MAX_SAMPLES = 2400;
  // Вход — серый кадр 80x60 (4800 точек): берём каждую вторую.
  static const size_t SAMPLE_STRIDE = 2;
  uint8_t previous_[MAX_SAMPLES] = {};
  size_t previousCount_ = 0;
  uint8_t warmupFrames_ = 0;
  bool initialized_ = false;
};