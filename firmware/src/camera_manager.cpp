#include "camera_manager.h"

#include <Arduino.h>
#include "freertos/task.h"
#include "camera_pins.h"
#include "config.h"
#include "config_defaults.h"

bool CameraManager::begin() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 16;
  config.fb_count = 1;

  Serial.printf("[BOOT] PSRAM %sdetectada\n", psramFound() ? "" : "no ");
  Serial.println("[BOOT] Llamando esp_camera_init()");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al iniciar la cámara: 0x%x\n", err);
    initialized_ = false;
    return false;
  }

  initialized_ = true;
  active_ = true;
  needsStabilization_ = true;
  return true;
}

camera_fb_t* CameraManager::capture() {
  if (!active_ && !begin()) {
    Serial.println("[CAM] No se pudo activar la camara");
    return nullptr;
  }

  if (!active_) {
    active_ = true;
  }

  if (needsStabilization_) {
    Serial.println("[CAM] Descartando frames iniciales (estabilizacion)");
    for (int i = 0; i < 3; ++i) {
      camera_fb_t* discarded = esp_camera_fb_get();
      if (discarded) {
        esp_camera_fb_return(discarded);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    needsStabilization_ = false;
  }

  Serial.println("[CAM] Capturando foto");
  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) {
    Serial.println("Error al capturar la foto.");
  } else {
    Serial.printf("[CAM] Foto capturada: %u bytes\n", frame->len);
  }
  return frame;
}

void CameraManager::release() {
  if (active_) {
    esp_camera_deinit();
    active_ = false;
  }
}

void CameraManager::flashOn() {
#if FLASH_MANUAL_HABILITADO
  digitalWrite(FLASH_LED_PIN, HIGH);
  if (FLASH_PRECAP_MS > 0) {
    vTaskDelay(pdMS_TO_TICKS(FLASH_PRECAP_MS));
  }
#endif
}

void CameraManager::flashOff() {
#if FLASH_MANUAL_HABILITADO
  digitalWrite(FLASH_LED_PIN, LOW);
#endif
}

bool CameraManager::isInitialized() const {
  return initialized_;
}

bool CameraManager::isActive() const {
  return active_;
}
