#pragma once

#include <Arduino.h>
#include "esp_camera.h"

class CameraService {
 public:
  bool begin();
  camera_fb_t *captureMotionFrame();
  void releaseMotionFrame(camera_fb_t *frame);
  camera_fb_t *takePhoto();
  void releasePhoto(camera_fb_t *frame);

 private:
  void restoreMotionMode();
  bool photoMode_ = false;
};