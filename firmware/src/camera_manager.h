#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include <stddef.h>
#include "esp_camera.h"

class CameraManager {
public:
  bool begin();
  camera_fb_t* capture();
  void release();
  void flashOn();
  void flashOff();
  bool isInitialized() const;
  bool isActive() const;

private:
  bool initialized_ = false;
  bool active_ = false;
  bool needsStabilization_ = true;
};

#endif
