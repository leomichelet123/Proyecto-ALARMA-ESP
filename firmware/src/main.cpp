
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "camera_manager.h"
#include "alarm_tasks.h"
#include "offline.h"
#include "firebase_client.h"
#include "config.h"
#include "config_defaults.h"

unsigned long ultimaAlarmaPIR1 = 0;
unsigned long ultimaAlarmaTest = 0;
volatile bool capturaOfflineEnCurso = false;
int ultimoEstadoPIR1 = LOW;
int estadoAnteriorPIR1 = LOW;
unsigned long ultimoPulsoPIR1 = 0;
unsigned long inicioLowPIR1 = 0;
bool sensorPIR1Armado = true;
bool modoOfflineActivo = false;
bool wifiDisponible = false;
unsigned long ultimoIntentoReconexionWiFi = 0;
unsigned int intentosReconexionWiFi = 0;

const unsigned long INTERVALO_RECONEXION_WIFI_MS = 1200;
// Mutex: evita que dos tareas intenten utilizar la cámara simultáneamente.
SemaphoreHandle_t mutexCamara = nullptr;
CameraManager camara;

// ---------- Prototipos ----------
void conectarWiFi();
bool inicializarCamara();
camera_fb_t* tomarFoto();
bool procesarAlarma(int sensorId, bool disparoPorSensor3v = false);
bool procesarCapturaManual(String& detalleResultado);
void esperarTarea(unsigned long tiempoMs);
void encenderFlashManual();
void apagarFlashManual();
void reintentarWiFiEnSegundoPlano();
void manejarEventoWiFi(WiFiEvent_t event, WiFiEventInfo_t info);

//Inicializa el sistema, incluyendo pines, almacenamiento offline, WiFi y cámara.
void setup() {

#ifdef DIAGNOSTICO_SERIE_SOLO
  return;
#endif
  // Modo pulsador: pin en LOW estable y sube a HIGH al aplicar 3.3V.
  pinMode(PIR1_PIN, INPUT_PULLDOWN);
#if FLASH_MANUAL_HABILITADO
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
#endif

  inicializarAlmacenamientoOffline();

  WiFi.onEvent(manejarEventoWiFi);
  if (!inicializarCamara()) {
    ESP.restart();
  }

  // Liberar la camara en reposo deja más RAM interna para TLS/Firebase.
  camara.release();

  mutexCamara = xSemaphoreCreateMutex();

  inicializarTareasAlarma();
}
void loop() {
  unsigned long ahora = millis();

#ifdef DIAGNOSTICO_SERIE_SOLO
  yield();
  return;
#endif

#if MODO_TEST_FOTOS_AUTOMATICO
  if ((ahora - ultimaAlarmaTest) > INTERVALO_TEST_FOTO_MS) {
    ultimaAlarmaTest = ahora;
    encolarEventoSensor(99, true);
  }
#endif

  // Core 1 solo lee el sensor. WiFi/Firebase se manejan exclusivamente
  // en tareaRed() del core 0 para no competir por WiFi.begin/disconnect.

  //Lectura del estado actual del sensor PIR1.
  int estadoPIR1 = PIR1_HABILITADO ? digitalRead(PIR1_PIN) : LOW;
  unsigned long ahoraPulso = millis();

  // Armado inicial y rearmado: requiere LOW estable para evitar disparos espurios.
  if (!sensorPIR1Armado && estadoPIR1 == LOW) {
    if (inicioLowPIR1 == 0) {
      inicioLowPIR1 = ahoraPulso;
    } else if ((ahoraPulso - inicioLowPIR1) >= TIEMPO_LIBERACION_SENSOR_MS) {
      sensorPIR1Armado = true;
    }
  }

  ultimoEstadoPIR1 = estadoPIR1;
  if (estadoPIR1 == HIGH) {
    ultimoPulsoPIR1 = ahoraPulso;
    inicioLowPIR1 = 0;
  } else {
    if (inicioLowPIR1 == 0) {
      inicioLowPIR1 = ahoraPulso;
    }
    if (!sensorPIR1Armado && (ahoraPulso - inicioLowPIR1) >= TIEMPO_LIBERACION_SENSOR_MS) {
      sensorPIR1Armado = true;
    }
  }

  if (sensorPIR1Armado && estadoAnteriorPIR1 == LOW && estadoPIR1 == HIGH &&
      (ahora - ultimaAlarmaPIR1) > TIEMPO_ENTRE_ALARMAS_MS) {
    ultimaAlarmaPIR1 = ahora;
    sensorPIR1Armado = false;
    encolarEventoSensor(1, true);
  }

  estadoAnteriorPIR1 = estadoPIR1;

  yield();
}

// ==================== WiFi ====================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void reintentarWiFiEnSegundoPlano() {
  unsigned long ahora = millis();
  if ((ahora - ultimoIntentoReconexionWiFi) < INTERVALO_RECONEXION_WIFI_MS) {
    return;
  }

  ultimoIntentoReconexionWiFi = ahora;
  intentosReconexionWiFi++;

  // Cada ciertos intentos hacemos un begin completo para recuperarnos
  // mas rapido cuando el AP se apaga/enciende o cambia de canal.
  if ((intentosReconexionWiFi % 4) == 0) {
    WiFi.disconnect(false, false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return;
  }

  WiFi.reconnect();
}

void manejarEventoWiFi(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiDisponible = true;
      intentosReconexionWiFi = 0;
      registrarSalidaModoOffline();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiDisponible = false;
      idToken = "";
      registrarEntradaModoOffline();
      break;
    default:
      break;
  }
}

bool inicializarCamara() {
  return camara.begin();
}

camera_fb_t* tomarFoto() {
  return camara.capture();
}

void esperarTarea(unsigned long tiempoMs) {
  vTaskDelay(pdMS_TO_TICKS(tiempoMs));
}

void encenderFlashManual() {
  camara.flashOn();
}

void apagarFlashManual() {
  camara.flashOff();
}

// ==================== Lógica principal de alarma ====================
bool procesarAlarma(int sensorId, bool disparoPorSensor3v) {
  bool usarFlash = disparoPorSensor3v;
  bool enlaceCaido = (!wifiDisponible || WiFi.status() != WL_CONNECTED);
  if (enlaceCaido) {
    registrarEntradaModoOffline();
    bool guardada = guardarAlarmaOffline(sensorId, false, "movimiento", usarFlash);
    return guardada;
  }

  if (!mutexCamara || xSemaphoreTake(mutexCamara, pdMS_TO_TICKS(15000)) != pdTRUE) {
    return false;
  }

  for (int i = 0; i < SECUENCIA_FOTOS_POR_EVENTO; i++) {
    if (usarFlash) {
      encenderFlashManual();
    }
    camera_fb_t* fb = tomarFoto();
    if (usarFlash) {
      apagarFlashManual();
    }
    if (!fb) {
      String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + "_f" + String(i + 1) + ".jpg";
      guardarAlarmaEnDatabase(sensorId, "", storagePath, false, "movimiento");
    } else {
      String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + "_f" + String(i + 1) + ".jpg";
      size_t lenFoto = fb->len;
      uint8_t* copiaFoto = (uint8_t*)malloc(lenFoto);
      if (!copiaFoto) {
        esp_camera_fb_return(fb);
        guardarAlarmaEnDatabase(sensorId, "", storagePath, false, "movimiento");
        continue;
      }

      memcpy(copiaFoto, fb->buf, lenFoto);
      esp_camera_fb_return(fb);

      // Igual que captura manual: liberar camara antes de TLS mejora estabilidad de upload.
      if (camara.isActive()) {
        camara.release();
      }

      String photoUrl = "";
      const int maxReintentosSubida = 3;
      for (int intentoSubida = 1; intentoSubida <= maxReintentosSubida; intentoSubida++) {
        photoUrl = subirBytesAStorage(copiaFoto, lenFoto, storagePath);
        if (photoUrl != "") {
          break;
        }
        if (intentoSubida < maxReintentosSubida) {
          idToken = "";
          if (!asegurarAutenticacionFirebase()) {
            break;
          }
          esperarTarea(180);
        }
      }
      if (photoUrl != "") {
        guardarAlarmaEnDatabase(sensorId, photoUrl, storagePath, false, "movimiento");
      } else {
        guardarFotoOfflineDesdeBuffer(sensorId, false, "movimiento", copiaFoto, lenFoto);
        free(copiaFoto);
        if (camara.isActive()) {
          camara.release();
        }
        xSemaphoreGive(mutexCamara);
        return false;
      }
      free(copiaFoto);
    }

    if (i < SECUENCIA_FOTOS_POR_EVENTO - 1) {
      esperarTarea(INTERVALO_ENTRE_FOTOS_MS);
    }
  }

  if (camara.isActive()) {
    camara.release();
  }

  xSemaphoreGive(mutexCamara);
  return true;
}

bool procesarCapturaManual(String& detalleResultado) {
  if (WiFi.status() != WL_CONNECTED) {
    detalleResultado = "Sin WiFi en dispositivo, reintentando";
    return false;
  }

  if (!asegurarAutenticacionFirebase()) {
    detalleResultado = "Sin autenticacion Firebase valida, reintentando";
    return false;
  }

  if (!mutexCamara || xSemaphoreTake(mutexCamara, pdMS_TO_TICKS(15000)) != pdTRUE) {
    detalleResultado = "Camara ocupada, reintentando";
    return false;
  }

  bool fotoSubidaOk = false;
  const int maxIntentosSubida = 3;
  encenderFlashManual();
  camera_fb_t* fb = tomarFoto();
  apagarFlashManual();

  if (fb) {
    size_t lenFoto = fb->len;
    uint8_t* copiaFoto = (uint8_t*)malloc(lenFoto);
    if (copiaFoto) {
      memcpy(copiaFoto, fb->buf, lenFoto);
      esp_camera_fb_return(fb);

      // Baja consumo/uso de memoria durante TLS para reducir cuelgues en upload manual.
      if (camara.isActive()) {
        camara.release();
      }

      String storagePath = "capturas/manual_" + String(millis()) + "_i1.jpg";
      for (int intentoSubida = 1; intentoSubida <= maxIntentosSubida; intentoSubida++) {
        String photoUrl = subirBytesAStorage(copiaFoto, lenFoto, storagePath);
        if (photoUrl != "") {
          guardarAlarmaEnDatabase(0, photoUrl, storagePath, true, "captura_manual");
          fotoSubidaOk = true;
          detalleResultado = "Captura manual registrada en historial";
          break;
        }
        if (intentoSubida < maxIntentosSubida) {
          idToken = "";
          asegurarAutenticacionFirebase();
          esperarTarea(180);
        }
      }

      free(copiaFoto);
    } else {
      esp_camera_fb_return(fb);
    }
  }

  if (!fotoSubidaOk) {
    String fallbackPath = "capturas/manual_" + String(millis()) + "_fallback.jpg";
    guardarAlarmaEnDatabase(0, "", fallbackPath, true, "captura_manual");
    detalleResultado = "Captura manual registrada sin foto (reintentada)";
  }

  if (camara.isActive()) {
    camara.release();
  }

  apagarFlashManual();
  xSemaphoreGive(mutexCamara);

  return true;
}