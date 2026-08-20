#include "offline_queue.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include "freertos/semphr.h"
#include "esp_system.h"
#include "camera_manager.h"
#include "config.h"

namespace {
const char* OFFLINE_QUEUE_FILE = "/offline_queue.txt";
constexpr int MAX_ALARMAS_OFFLINE = 2;
constexpr int MAX_FOTOS_POR_ALARMA_OFFLINE = 1;

String rutaMetaOffline(const String& eventId) {
  return "/off_" + eventId + "_meta.json";
}

String rutaFotoOffline(const String& eventId, int indiceFoto) {
  return "/off_" + eventId + "_p" + String(indiceFoto) + ".jpg";
}
}

extern String idToken;
extern bool wifiDisponible;
extern bool modoOfflineActivo;
extern SemaphoreHandle_t mutexCamara;
extern camera_fb_t* tomarFoto();
extern void encenderFlashManual();
extern void apagarFlashManual();
extern bool asegurarAutenticacionFirebase();
extern String subirBytesAStorage(const uint8_t* data, size_t len, const String& storagePath);
extern void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante, const String& tipoEvento);

bool inicializarAlmacenamientoOffline() {
  if (!SPIFFS.begin(true)) {
    return false;
  }

  if (!SPIFFS.exists(OFFLINE_QUEUE_FILE)) {
    File queue = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_WRITE);
    if (queue) {
      queue.close();
    }
  }

  return true;
}

String generarIdEventoOffline(int sensorId, const String& tipoEvento) {
  String id = String(millis()) + "_" + String(sensorId) + "_" + String((uint32_t)esp_random(), HEX);
  if (tipoEvento.length() > 0) {
    id += "_" + tipoEvento;
  }
  return id;
}

bool escribirArchivoSPIFFS(const String& ruta, const uint8_t* datos, size_t longitud) {
  File archivo = SPIFFS.open(ruta, FILE_WRITE);
  if (!archivo) {
    return false;
  }

  size_t escritos = archivo.write(datos, longitud);
  archivo.close();
  return escritos == longitud;
}

int contarAlarmasOfflinePendientes() {
  File cola = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_READ);
  if (!cola) {
    return 0;
  }

  int cantidad = 0;
  while (cola.available()) {
    String linea = cola.readStringUntil('\n');
    linea.trim();
    if (linea.length() > 0) {
      cantidad++;
    }
  }

  cola.close();
  return cantidad;
}

bool guardarMetaEventoOffline(const String& eventId, int sensorId, bool importante, const String& tipoEvento) {
  DynamicJsonDocument doc(256);
  doc["eventId"] = eventId;
  doc["sensorId"] = sensorId;
  doc["importante"] = importante;
  doc["tipoEvento"] = tipoEvento;
  doc["fotos"] = MAX_FOTOS_POR_ALARMA_OFFLINE;
  doc["createdAtMs"] = millis();

  File meta = SPIFFS.open(rutaMetaOffline(eventId), FILE_WRITE);
  if (!meta) {
    return false;
  }

  if (serializeJson(doc, meta) == 0) {
    meta.close();
    return false;
  }

  meta.close();
  return true;
}

bool agregarEventoAColaOffline(const String& eventId) {
  if (contarAlarmasOfflinePendientes() >= MAX_ALARMAS_OFFLINE) {
    return false;
  }

  File cola = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_APPEND);
  if (!cola) {
    return false;
  }

  cola.println(eventId);
  cola.close();
  return true;
}

bool obtenerPrimerEventoColaOffline(String& eventId) {
  File cola = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_READ);
  if (!cola) {
    return false;
  }

  while (cola.available()) {
    String linea = cola.readStringUntil('\n');
    linea.trim();
    if (linea.length() > 0) {
      eventId = linea;
      cola.close();
      return true;
    }
  }

  cola.close();
  return false;
}

bool eliminarPrimerEventoColaOffline() {
  File cola = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_READ);
  if (!cola) {
    return false;
  }

  String restante;
  bool saltoPrimera = false;
  while (cola.available()) {
    String linea = cola.readStringUntil('\n');
    linea.trim();
    if (linea.length() == 0) {
      continue;
    }
    if (!saltoPrimera) {
      saltoPrimera = true;
      continue;
    }
    restante += linea + "\n";
  }
  cola.close();

  File salida = SPIFFS.open(OFFLINE_QUEUE_FILE, FILE_WRITE);
  if (!salida) {
    return false;
  }
  salida.print(restante);
  salida.close();
  return true;
}

bool guardarAlarmaOffline(int sensorId, bool importante, const String& tipoEvento, bool usarFlash) {
  if (contarAlarmasOfflinePendientes() >= MAX_ALARMAS_OFFLINE) {
    Serial.println("[OFFLINE] Cola llena, no se puede guardar otra alarma sin internet.");
    return false;
  }

  if (!mutexCamara || xSemaphoreTake(mutexCamara, pdMS_TO_TICKS(15000)) != pdTRUE) {
    Serial.println("[OFFLINE] Camara ocupada, no se pudo guardar la alarma.");
    return false;
  }

  String eventId = generarIdEventoOffline(sensorId, tipoEvento);
  if (!guardarMetaEventoOffline(eventId, sensorId, importante, tipoEvento)) {
    xSemaphoreGive(mutexCamara);
    return false;
  }

  for (int i = 1; i <= MAX_FOTOS_POR_ALARMA_OFFLINE; i++) {
    Serial.printf("[OFFLINE] Capturando foto %d/%d para evento local\n", i, MAX_FOTOS_POR_ALARMA_OFFLINE);
    if (usarFlash) {
      encenderFlashManual();
    }
    camera_fb_t* fb = tomarFoto();
    if (usarFlash) {
      apagarFlashManual();
    }
    if (!fb) {
      Serial.println("[OFFLINE] No se pudo capturar foto para guardar localmente.");
      xSemaphoreGive(mutexCamara);
      return false;
    }

    bool ok = escribirArchivoSPIFFS(rutaFotoOffline(eventId, i), fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (!ok) {
      Serial.println("[OFFLINE] No se pudo escribir la foto en SPIFFS.");
      xSemaphoreGive(mutexCamara);
      return false;
    }
  }

  if (usarFlash) {
    apagarFlashManual();
  }

  if (!agregarEventoAColaOffline(eventId)) {
    Serial.println("[OFFLINE] No se pudo agregar el evento a la cola.");
    xSemaphoreGive(mutexCamara);
    return false;
  }

  xSemaphoreGive(mutexCamara);
  Serial.printf("[OFFLINE] Evento guardado localmente: %s\n", eventId.c_str());
  return true;
}

bool guardarFotoOfflineDesdeBuffer(int sensorId, bool importante, const String& tipoEvento, const uint8_t* datos, size_t longitud) {
  if (!datos || longitud == 0 || contarAlarmasOfflinePendientes() >= MAX_ALARMAS_OFFLINE) {
    Serial.println("[OFFLINE] No se pudo encolar la foto capturada.");
    return false;
  }

  String eventId = generarIdEventoOffline(sensorId, tipoEvento);
  if (!guardarMetaEventoOffline(eventId, sensorId, importante, tipoEvento)) {
    return false;
  }

  String rutaFoto = rutaFotoOffline(eventId, 1);
  if (!escribirArchivoSPIFFS(rutaFoto, datos, longitud) || !agregarEventoAColaOffline(eventId)) {
    SPIFFS.remove(rutaFoto);
    SPIFFS.remove(rutaMetaOffline(eventId));
    return false;
  }

  Serial.printf("[OFFLINE] Foto capturada guardada localmente: %s\n", eventId.c_str());
  return true;
}

bool leerMetaEventoOffline(const String& eventId, DynamicJsonDocument& meta) {
  File archivo = SPIFFS.open(rutaMetaOffline(eventId), FILE_READ);
  if (!archivo) {
    return false;
  }

  DeserializationError err = deserializeJson(meta, archivo);
  archivo.close();
  return !err;
}

bool sincronizarEventoOffline(const String& eventId) {
  DynamicJsonDocument meta(256);
  if (!leerMetaEventoOffline(eventId, meta)) {
    Serial.printf("[OFFLINE] No se pudo leer meta de %s\n", eventId.c_str());
    return false;
  }

  int sensorId = meta["sensorId"] | 1;
  bool importante = meta["importante"] | false;
  String tipoEvento = meta["tipoEvento"] | "movimiento";
  int fotosEvento = meta["fotos"] | MAX_FOTOS_POR_ALARMA_OFFLINE;
  if (fotosEvento < 1) {
    fotosEvento = 1;
  }

  for (int i = 1; i <= fotosEvento; i++) {
    String rutaFoto = rutaFotoOffline(eventId, i);
    File foto = SPIFFS.open(rutaFoto, FILE_READ);
    if (!foto) {
      Serial.printf("[OFFLINE] Falta foto %d de %s\n", i, eventId.c_str());
      return false;
    }

    size_t lenFoto = foto.size();
    uint8_t* buffer = (uint8_t*)malloc(lenFoto);
    if (!buffer) {
      foto.close();
      return false;
    }

    size_t leidos = foto.read(buffer, lenFoto);
    foto.close();
    if (leidos != lenFoto) {
      free(buffer);
      return false;
    }

    String storagePath = "offline/" + eventId + "/photo_" + String(i) + ".jpg";
    String photoUrl = subirBytesAStorage(buffer, lenFoto, storagePath);
    free(buffer);
    if (photoUrl == "") {
      Serial.printf("[OFFLINE] Fallo la subida de foto %d de %s\n", i, eventId.c_str());
      return false;
    }

    guardarAlarmaEnDatabase(sensorId, photoUrl, storagePath, importante, tipoEvento);
  }

  SPIFFS.remove(rutaMetaOffline(eventId));
  for (int i = 1; i <= fotosEvento; i++) {
    SPIFFS.remove(rutaFotoOffline(eventId, i));
  }
  Serial.printf("[OFFLINE] Evento sincronizado: %s\n", eventId.c_str());
  return true;
}

void registrarEntradaModoOffline(const char* motivo) {
  if (!modoOfflineActivo) {
    modoOfflineActivo = true;
    Serial.printf("[OFFLINE] ENTRE MODO OFFLINE (%s)\n", motivo);
  }
}

void registrarSalidaModoOffline() {
  if (modoOfflineActivo) {
    modoOfflineActivo = false;
    Serial.println("[OFFLINE] Sali de modo offline");
  }
}

bool sincronizarColaOffline() {
  if (!wifiDisponible || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!asegurarAutenticacionFirebase()) {
    return false;
  }

  String eventId;
  if (!obtenerPrimerEventoColaOffline(eventId)) {
    return true;
  }

  if (!sincronizarEventoOffline(eventId)) {
    Serial.printf("[OFFLINE] Se detuvo la sincronizacion en %s\n", eventId.c_str());
    return false;
  }

  eliminarPrimerEventoColaOffline();
  return true;
}
