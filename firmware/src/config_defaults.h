#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

// AI-Thinker ESP32-CAM: flash LED integrado en GPIO4.
#ifndef FLASH_MANUAL_HABILITADO
#define FLASH_MANUAL_HABILITADO 1
#endif

#ifndef FLASH_LED_PIN
#define FLASH_LED_PIN 4
#endif

#ifndef FLASH_PRECAP_MS
#define FLASH_PRECAP_MS 120
#endif

#endif
