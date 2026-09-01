#ifndef FIREBASE_CLIENT_H
#define FIREBASE_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

// Token de sesión del dispositivo (se obtiene al autenticarse contra Firebase).
extern String idToken;

void configurarClienteSeguro(WiFiClientSecure& client);
bool asegurarDnsHost(const char* host, int maxIntentos = 3);
String construirUrlDbConAuth(const String& urlBase);

bool autenticarDispositivo();
bool asegurarAutenticacionFirebase();

String codificarNombreObjetoStorage(const String& nombreArchivo);
String generarTokenDescarga();
String subirBytesAStorage(const uint8_t* data, size_t len, const String& nombreArchivo, bool* authFallo = nullptr);

bool postJsonEnDatabase(const String& url, const String& body);
bool putJsonEnDatabase(const String& url, const String& body);
bool patchJsonEnDatabase(const String& url, const String& body);

bool guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante, const String& tipoEvento = "movimiento");

void publicarEstadoDispositivo(bool forzar);
bool publicarEstadoDispositivoConReintento(const String& url, const String& body);

void revisarComandoCapturaManual();

#endif
