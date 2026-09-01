#include "offline.h"

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include "freertos/semphr.h"
#include "camera_manager.h"
#include "config.h"

extern bool wifiDisponible;
extern bool modoOfflineActivo;
extern SemaphoreHandle_t mutexCamara;
extern camera_fb_t* tomarFoto();
extern void encenderFlashManual();
extern void apagarFlashManual();
extern String subirBytesAStorage(const uint8_t* data, size_t len, const String& storagePath);
extern bool guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante, const String& tipoEvento);
extern bool asegurarAutenticacionFirebase();

namespace {
constexpr int MAX_ALARMAS_OFFLINE = 2;
constexpr int MAX_FOTOS_POR_ALARMA_OFFLINE = 1;

String rutaMetaOffline(int slot) {
  return "/off_" + String(slot) + "_meta.json";
}

String rutaFotoOffline(int slot) {
  return "/off_" + String(slot) + ".jpg";
}

int buscarSlotLibre() {
  for (int slot = 0; slot < MAX_ALARMAS_OFFLINE; ++slot) {
    if (!SPIFFS.exists(rutaMetaOffline(slot))) {
      return slot;
    }
  }
  return -1;
}

int buscarPrimerSlotPendiente() {
  for (int slot = 0; slot < MAX_ALARMAS_OFFLINE; ++slot) {
    if (SPIFFS.exists(rutaMetaOffline(slot))) {
      return slot;
    }
  }
  return -1;
}

void limpiarSlot(int slot) {
  SPIFFS.remove(rutaMetaOffline(slot));
  SPIFFS.remove(rutaFotoOffline(slot));
}

bool escribirArchivoSPIFFS(const String& ruta, const uint8_t* datos, size_t longitud) {
  // Evita arrastrar contenido viejo si quedo un archivo huerfano en el slot.
  if (SPIFFS.exists(ruta)) {
    SPIFFS.remove(ruta);
  }

  File archivo = SPIFFS.open(ruta, FILE_WRITE);
  if (!archivo) {
    return false;
  }

  size_t escritos = archivo.write(datos, longitud);
  archivo.close();
  return escritos == longitud;
}

bool guardarMetaEventoOffline(int slot, int sensorId, bool importante, const String& tipoEvento) {
  DynamicJsonDocument doc(256);
  doc["slot"] = slot;
  doc["sensorId"] = sensorId;
  doc["importante"] = importante;
  doc["tipoEvento"] = tipoEvento;
  doc["fotos"] = MAX_FOTOS_POR_ALARMA_OFFLINE;
  doc["createdAtMs"] = millis();

  File meta = SPIFFS.open(rutaMetaOffline(slot), FILE_WRITE);
  if (!meta) {
    return false;
  }

  bool ok = serializeJson(doc, meta) > 0;
  meta.close();
  return ok;
}

bool leerMetaEventoOffline(int slot, DynamicJsonDocument& meta) {
  File archivo = SPIFFS.open(rutaMetaOffline(slot), FILE_READ);
  if (!archivo) {
    return false;
  }

  DeserializationError error = deserializeJson(meta, archivo);
  archivo.close();
  return !error;
}

bool guardarFotoEnSlot(int slot, const uint8_t* datos, size_t longitud) {
  return escribirArchivoSPIFFS(rutaFotoOffline(slot), datos, longitud);
}

bool sincronizarEventoOffline(int slot) {
  DynamicJsonDocument meta(256);
  if (!leerMetaEventoOffline(slot, meta)) {
    limpiarSlot(slot);
    return true;
  }

  int sensorId = meta["sensorId"] | 1;
  bool importante = meta["importante"] | false;
  String tipoEvento = meta["tipoEvento"] | "movimiento";
  String tipoEventoOffline = tipoEvento;
  if (!tipoEventoOffline.endsWith(" (offline)")) {
    tipoEventoOffline += " (offline)";
  }

  File foto = SPIFFS.open(rutaFotoOffline(slot), FILE_READ);
  if (!foto) {
    limpiarSlot(slot);
    return true;
  }

  size_t longitud = foto.size();
  if (longitud == 0) {
    foto.close();
    limpiarSlot(slot);
    return true;
  }

  uint8_t* datos = static_cast<uint8_t*>(malloc(longitud));
  if (!datos) {
    foto.close();
    return false;
  }

  size_t leidos = foto.read(datos, longitud);
  foto.close();
  if (leidos != longitud) {
    free(datos);
    limpiarSlot(slot);
    return true;
  }

  String storagePath = "offline/slot_" + String(slot) + "/photo_1.jpg";
  String photoUrl = subirBytesAStorage(datos, longitud, storagePath);
  free(datos);

  if (photoUrl == "") {
    return false;
  }

  if (!guardarAlarmaEnDatabase(sensorId, photoUrl, storagePath, importante, tipoEventoOffline)) {
    return false;
  }

  limpiarSlot(slot);
  return true;
}
}

bool inicializarAlmacenamientoOffline() {
  if (!SPIFFS.begin(true)) {
    return false;
  }

  // Repara archivos parciales de un reset/corte para que no bloqueen la cola.
  for (int slot = 0; slot < MAX_ALARMAS_OFFLINE; ++slot) {
    bool tieneMeta = SPIFFS.exists(rutaMetaOffline(slot));
    bool tieneFoto = SPIFFS.exists(rutaFotoOffline(slot));
    if (tieneMeta != tieneFoto) {
      limpiarSlot(slot);
    }
  }

  return true;
}

int contarAlarmasOfflinePendientes() {
  int pendientes = 0;
  for (int slot = 0; slot < MAX_ALARMAS_OFFLINE; ++slot) {
    if (SPIFFS.exists(rutaMetaOffline(slot))) {
      ++pendientes;
    }
  }
  return pendientes;
}

bool guardarAlarmaOffline(int sensorId, bool importante, const String& tipoEvento, bool usarFlash) {
  int slot = buscarSlotLibre();
  if (slot < 0) {
    return false;
  }

  if (!mutexCamara || xSemaphoreTake(mutexCamara, pdMS_TO_TICKS(15000)) != pdTRUE) {
    return false;
  }

  if (!guardarMetaEventoOffline(slot, sensorId, importante, tipoEvento)) {
    xSemaphoreGive(mutexCamara);
    return false;
  }

  if (usarFlash) {
    encenderFlashManual();
  }
  camera_fb_t* foto = tomarFoto();
  if (usarFlash) {
    apagarFlashManual();
  }

  if (!foto) {
    limpiarSlot(slot);
    xSemaphoreGive(mutexCamara);
    return false;
  }

  bool guardada = guardarFotoEnSlot(slot, foto->buf, foto->len);
  esp_camera_fb_return(foto);
  if (!guardada) {
    limpiarSlot(slot);
    xSemaphoreGive(mutexCamara);
    return false;
  }

  if (usarFlash) {
    apagarFlashManual();
  }
  xSemaphoreGive(mutexCamara);
  return true;
}

bool guardarFotoOfflineDesdeBuffer(int sensorId, bool importante, const String& tipoEvento, const uint8_t* datos, size_t longitud) {
  if (!datos || longitud == 0) {
    return false;
  }

  int slot = buscarSlotLibre();
  if (slot < 0 || !guardarMetaEventoOffline(slot, sensorId, importante, tipoEvento) ||
      !guardarFotoEnSlot(slot, datos, longitud)) {
    if (slot >= 0) {
      limpiarSlot(slot);
    }
    return false;
  }
  return true;
}

void registrarEntradaModoOffline() {
  if (!modoOfflineActivo) {
    modoOfflineActivo = true;
  }
}

void registrarSalidaModoOffline() {
  if (modoOfflineActivo) {
    modoOfflineActivo = false;
  }
}

bool sincronizarColaOffline() {
  if (!wifiDisponible || WiFi.status() != WL_CONNECTED || !asegurarAutenticacionFirebase()) {
    return false;
  }

  // Procesa todos los pendientes en la misma reconexion para evitar
  // que las fotos offline lleguen con mucho atraso.
  for (int i = 0; i < MAX_ALARMAS_OFFLINE; ++i) {
    int slot = buscarPrimerSlotPendiente();
    if (slot < 0) {
      return true;
    }

    if (!sincronizarEventoOffline(slot)) {
      return false;
    }
  }

  return true;
}