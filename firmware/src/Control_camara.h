#ifndef CONTROL_CAMARA_H
#define CONTROL_CAMARA_H

#include <stddef.h>
#include "esp_camera.h"

// Administra la inicializacion, captura y liberacion de la camara ESP32-CAM,
// junto con el control del flash y el estado del dispositivo.
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
