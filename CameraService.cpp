#include "CameraService.h"

bool CameraService::initWith(pixformat_t format, framesize_t size, Mode mode) {
  if (mode_ != Mode::Off) {
    esp_camera_deinit();
    mode_ = Mode::Off;
    delay(20);
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
  config.pixel_format = format;
  config.frame_size = size;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    lastError_ = "esp_camera_init: 0x" + String(static_cast<unsigned>(result), HEX);
    return false;
  }
  lastError_ = "";
  mode_ = mode;
  return true;
}

bool CameraService::begin() {
  return enterMotionMode();
}

void CameraService::end() {
  if (mode_ != Mode::Off) esp_camera_deinit();
  mode_ = Mode::Off;
}

camera_fb_t *CameraService::captureMotionFrame() {
  if (mode_ != Mode::Motion) return nullptr;
  return esp_camera_fb_get();
}

bool CameraService::enterMotionMode() {
  if (mode_ == Mode::Motion) return true;
  return initWith(PIXFORMAT_GRAYSCALE, FRAMESIZE_QQVGA, Mode::Motion);
}

bool CameraService::enterPhotoMode() {
  if (mode_ == Mode::Photo) return true;
  // Без PSRAM буфер VGA JPEG может не поместиться во внутреннюю память.
  const framesize_t size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  if (!initWith(PIXFORMAT_JPEG, size, Mode::Photo)) return false;
  // Первые кадры после старта сенсора тёмные: даём автоэкспозиции подстроиться.
  for (uint8_t i = 0; i < 3; ++i) {
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup != nullptr) esp_camera_fb_return(warmup);
    delay(60);
  }
  return true;
}

camera_fb_t *CameraService::capturePhoto() {
  if (mode_ != Mode::Photo) return nullptr;
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame != nullptr && frame->format != PIXFORMAT_JPEG) {
    esp_camera_fb_return(frame);
    lastError_ = "кадр получен не в формате JPEG";
    return nullptr;
  }
  if (frame == nullptr) lastError_ = "esp_camera_fb_get вернул пустой кадр";
  return frame;
}

void CameraService::releaseFrame(camera_fb_t *frame) {
  if (frame != nullptr) esp_camera_fb_return(frame);
}
