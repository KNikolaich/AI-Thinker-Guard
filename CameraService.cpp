#include "CameraService.h"

bool CameraService::begin() {
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
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size = FRAMESIZE_QQVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  return esp_camera_init(&config) == ESP_OK;
}

camera_fb_t *CameraService::captureMotionFrame() {
  if (photoMode_) return nullptr;
  return esp_camera_fb_get();
}

void CameraService::releaseMotionFrame(camera_fb_t *frame) {
  if (frame != nullptr) esp_camera_fb_return(frame);
}

camera_fb_t *CameraService::takePhoto() {
  if (photoMode_) return nullptr;
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) return nullptr;
  photoMode_ = true;
  sensor->set_framesize(sensor, FRAMESIZE_VGA);
  sensor->set_pixformat(sensor, PIXFORMAT_JPEG);
  delay(180);
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) restoreMotionMode();
  return frame;
}

void CameraService::releasePhoto(camera_fb_t *frame) {
  if (frame != nullptr) esp_camera_fb_return(frame);
  restoreMotionMode();
}

void CameraService::restoreMotionMode() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_QQVGA);
    sensor->set_pixformat(sensor, PIXFORMAT_GRAYSCALE);
  }
  photoMode_ = false;
  delay(40);
}