#pragma once

#include <Arduino.h>
#include "esp_camera.h"

// Драйвер esp32-camera выделяет буферы и настраивает DMA под формат и размер,
// заданные в esp_camera_init(). Переключить GRAYSCALE QQVGA -> JPEG VGA через
// set_pixformat()/set_framesize() нельзя: кадр остаётся в прежнем формате и
// не помещается в буфер. Поэтому режимы переключаются переинициализацией.
class CameraService {
 public:
  enum class Mode : uint8_t { Off, Motion, Photo };

  bool begin();  // Инициализация в режиме детектора движения.
  void end();
  bool isReady() const { return mode_ != Mode::Off; }
  Mode mode() const { return mode_; }
  const String &lastError() const { return lastError_; }

  camera_fb_t *captureMotionFrame();
  bool enterPhotoMode();
  camera_fb_t *capturePhoto();
  bool enterMotionMode();
  void releaseFrame(camera_fb_t *frame);

 private:
  bool initWith(pixformat_t format, framesize_t size, Mode mode);
  Mode mode_ = Mode::Off;
  String lastError_;
};
