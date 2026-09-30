#include "CameraService.h"

#include "esp_jpg_decode.h"
#include <esp_heap_caps.h>

bool CameraService::begin() {
  end();
  if (gray_ == nullptr) {
    gray_ = static_cast<uint8_t *>(heap_caps_malloc(MAX_GRAY_PIXELS, MALLOC_CAP_SPIRAM));
    if (gray_ == nullptr) gray_ = static_cast<uint8_t *>(malloc(MAX_GRAY_PIXELS));
    if (gray_ == nullptr) {
      lastError_ = "нет памяти под кадр детектора";
      return false;
    }
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = 12;
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;  // всегда самый свежий кадр
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    lastError_ = "esp_camera_init: 0x" + String(static_cast<unsigned>(result), HEX);
    return false;
  }
  lastError_ = "";
  ready_ = true;

  // Первые кадры после старта сенсора тёмные: даём автоэкспозиции подстроиться.
  for (uint8_t i = 0; i < 4; ++i) {
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup != nullptr) esp_camera_fb_return(warmup);
    delay(50);
  }
  return true;
}

void CameraService::end() {
  if (ready_) esp_camera_deinit();
  ready_ = false;
}

size_t CameraService::jpegRead(void *arg, size_t index, uint8_t *buf, size_t len) {
  const camera_fb_t *frame = static_cast<CameraService *>(arg)->decoding_;
  if (frame == nullptr || index >= frame->len) return 0;
  if (index + len > frame->len) len = frame->len - index;
  if (buf != nullptr) memcpy(buf, frame->buf + index, len);
  return len;
}

bool CameraService::grayWrite(void *arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint8_t *data) {
  if (data == nullptr) return true;  // начало/конец декодирования
  CameraService *self = static_cast<CameraService *>(arg);
  for (uint16_t row = 0; row < h; ++row) {
    const uint16_t gy = y + row;
    if (gy >= self->grayHeight_) break;
    for (uint16_t col = 0; col < w; ++col) {
      const uint16_t gx = x + col;
      if (gx >= self->grayWidth_) break;
      const uint8_t *p = data + (static_cast<size_t>(row) * w + col) * 3;
      self->gray_[static_cast<size_t>(gy) * self->grayWidth_ + gx] =
          static_cast<uint8_t>((p[0] + 2 * p[1] + p[2]) >> 2);
    }
  }
  return true;
}

bool CameraService::captureMotionSample(const uint8_t *&pixels, size_t &length) {
  if (!ready_) return false;
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    lastError_ = "esp_camera_fb_get вернул пустой кадр";
    return false;
  }
  grayWidth_ = frame->width / 8;
  grayHeight_ = frame->height / 8;
  bool ok = frame->format == PIXFORMAT_JPEG && grayWidth_ > 0 && grayHeight_ > 0 &&
            static_cast<size_t>(grayWidth_) * grayHeight_ <= MAX_GRAY_PIXELS;
  if (ok) {
    decoding_ = frame;
    ok = esp_jpg_decode(frame->len, JPG_SCALE_8X, jpegRead, grayWrite, this) == ESP_OK;
    decoding_ = nullptr;
  }
  esp_camera_fb_return(frame);
  if (!ok) {
    // Битый кадр — не повод считать камеру сломанной, просто пропускаем.
    length = 0;
    pixels = gray_;
    return true;
  }
  pixels = gray_;
  length = static_cast<size_t>(grayWidth_) * grayHeight_;
  return true;
}

camera_fb_t *CameraService::capturePhoto() {
  if (!ready_) return nullptr;
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    lastError_ = "esp_camera_fb_get вернул пустой кадр";
    return nullptr;
  }
  if (frame->format != PIXFORMAT_JPEG) {
    esp_camera_fb_return(frame);
    lastError_ = "кадр получен не в формате JPEG";
    return nullptr;
  }
  return frame;
}

void CameraService::releaseFrame(camera_fb_t *frame) {
  if (frame != nullptr) esp_camera_fb_return(frame);
}
