#ifndef OFFLINE_QUEUE_H
#define OFFLINE_QUEUE_H

#include <Arduino.h>

bool inicializarAlmacenamientoOffline();
int contarAlarmasOfflinePendientes();
bool guardarAlarmaOffline(int sensorId, bool importante, const String& tipoEvento, bool usarFlash);
bool guardarFotoOfflineDesdeBuffer(int sensorId, bool importante, const String& tipoEvento, const uint8_t* datos, size_t longitud);
bool sincronizarColaOffline();
void registrarEntradaModoOffline(const char* motivo);
void registrarSalidaModoOffline();

#endif
