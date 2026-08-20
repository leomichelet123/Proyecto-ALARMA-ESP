#ifndef OFFLINE_QUEUE_H
#define OFFLINE_QUEUE_H

#include <stddef.h>
#include <ArduinoJson.h>

bool inicializarAlmacenamientoOffline();
int contarAlarmasOfflinePendientes();
String generarIdEventoOffline(int sensorId, const String& tipoEvento);
bool escribirArchivoSPIFFS(const String& ruta, const uint8_t* datos, size_t longitud);
bool guardarMetaEventoOffline(const String& eventId, int sensorId, bool importante, const String& tipoEvento);
bool agregarEventoAColaOffline(const String& eventId);
bool obtenerPrimerEventoColaOffline(String& eventId);
bool eliminarPrimerEventoColaOffline();
bool guardarAlarmaOffline(int sensorId, bool importante, const String& tipoEvento, bool usarFlash);
bool guardarFotoOfflineDesdeBuffer(int sensorId, bool importante, const String& tipoEvento, const uint8_t* datos, size_t longitud);
bool sincronizarColaOffline();
bool sincronizarEventoOffline(const String& eventId);
bool leerMetaEventoOffline(const String& eventId, DynamicJsonDocument& meta);
void registrarEntradaModoOffline(const char* motivo);
void registrarSalidaModoOffline();

#endif
