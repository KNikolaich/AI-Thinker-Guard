#pragma once

#include <Arduino.h>
#include "esp_camera.h"

// Камера инициализируется ОДИН раз в режиме JPEG (VGA с PSRAM, QVGA без неё)
// и больше не переинициализируется. Детектор движения получает уменьшенный
// в 8 раз чёрно-белый кадр, декодированный из того же JPEG.
//
// Почему так: DMA-буфер драйвера (~32 КБ) должен лежать во внутренней RAM.
// После запуска Wi-Fi и TLS непрерывного блока такого размера может уже
// не быть, и переинициализация камеры для каждого снимка падала с
// "DMA buffer malloc failed".
class CameraService {
 public:
  bool begin();
  void end();
  bool isReady() const { return ready_; }
  const String &lastError() const { return lastError_; }

  // Кадр для детектора: оттенки серого, ширина/высота = кадр / 8.
  bool captureMotionSample(const uint8_t *&pixels, size_t &length);
  // JPEG для отправки; вернуть через releaseFrame().
  camera_fb_t *capturePhoto();
  void releaseFrame(camera_fb_t *frame);

  static const size_t MAX_GRAY_PIXELS = 80 * 60;

 private:
  static size_t jpegRead(void *arg, size_t index, uint8_t *buf, size_t len);
  static bool grayWrite(void *arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t *data);

  bool ready_ = false;
  String lastError_;
  uint8_t *gray_ = nullptr;  // в PSRAM, если есть: внутренняя RAM нужна Wi-Fi и TLS
  uint16_t grayWidth_ = 0;
  uint16_t grayHeight_ = 0;
  const camera_fb_t *decoding_ = nullptr;
};
