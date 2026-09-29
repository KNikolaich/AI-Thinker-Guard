#include "MotionDetector.h"

bool MotionDetector::detect(const uint8_t *pixels, size_t length) {
  if (pixels == nullptr || length == 0) return false;

  size_t sampleCount = 0;
  uint16_t changed = 0;
  for (size_t offset = 0; offset < length && sampleCount < MAX_SAMPLES;
       offset += SAMPLE_STRIDE, ++sampleCount) {
    const uint8_t current = pixels[offset];
    if (initialized_ && sampleCount < previousCount_) {
      const int difference = abs(static_cast<int>(current) -
                                 static_cast<int>(previous_[sampleCount]));
      if (difference >= 22) ++changed;
    }
    previous_[sampleCount] = current;
  }

  const bool sameShape = initialized_ && sampleCount == previousCount_;
  const bool ready = sameShape && warmupFrames_ >= 2;
  const bool detected = ready && sampleCount > 0 &&
                        static_cast<float>(changed) / sampleCount >= 0.12f;

  previousCount_ = sampleCount;
  initialized_ = true;
  if (warmupFrames_ < 3) ++warmupFrames_;
  return detected;
}

void MotionDetector::reset() {
  previousCount_ = 0;
  warmupFrames_ = 0;
  initialized_ = false;
}