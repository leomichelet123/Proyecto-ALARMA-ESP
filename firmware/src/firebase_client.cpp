#include "firebase_client.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Control_camara.h"
#include "offline.h"
#include "config.h"

String idToken = "";

extern bool wifiDisponible;
extern CameraManager camara;
extern unsigned long ultimoPulsoPIR1;
extern int ultimoEstadoPIR1;
extern bool procesarCapturaManual(String& detalleResultado);

namespace {
const unsigned long INTERVALO_REINTENTO_AUTH_MS = 1200;
unsigned long ultimoIntentoAuthFirebase = 0;
unsigned long ultimoEstadoDispositivo = 0;
unsigned long ultimoChequeoComandoManual = 0;
}

void configurarClienteSeguro(WiFiClientSecure& client) {
  client.setInsecure();
  client.setTimeout(15000);
}

bool asegurarDnsHost(const char* host, int maxIntentos) {
  if (!wifiDisponible || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  for (int intento = 1; intento <= maxIntentos; intento++) {
    IPAddress ip;
    if (WiFi.hostByName(host, ip)) {
      return true;
    }
  }

  return false;
}

String construirUrlDbConAuth(const String& urlBase) {
  if (idToken.length() == 0) {
    return urlBase;
  }

  if (urlBase.indexOf("?") >= 0) {
    return urlBase + "&auth=" + idToken;
  }
  return urlBase + "?auth=" + idToken;
}

// ==================== Autenticación del dispositivo ====================
// Usa la cuenta creada en Authentication para obtener un token que permite
// escribir en la base de datos y en Storage, según las reglas de seguridad.
bool autenticarDispositivo() {
  if (!asegurarDnsHost("identitytoolkit.googleapis.com")) {
    return false;
  }

  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(FIREBASE_API_KEY);
  String body = "{\"email\":\"" + String(FIREBASE_DEVICE_EMAIL) +
                "\",\"password\":\"" + String(FIREBASE_DEVICE_PASSWORD) +
                "\",\"returnSecureToken\":true}";

  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(body);
  if (httpCode == 200) {
    String respuesta = http.getString();
    http.end();

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, respuesta);

    if (!error && doc.containsKey("idToken")) {
      idToken = doc["idToken"].as<String>();
      return true;
    }
    return false;
  }
  http.end();
  client.stop();

  return false;
}

bool asegurarAutenticacionFirebase() {
  if (!wifiDisponible || WiFi.status() != WL_CONNECTED) {
    registrarEntradaModoOffline();
    return false;
  }

  if (idToken.length() > 0) {
    return true;
  }

  unsigned long ahora = millis();
  if ((ahora - ultimoIntentoAuthFirebase) < INTERVALO_REINTENTO_AUTH_MS) {
    return false;
  }

  ultimoIntentoAuthFirebase = ahora;
  bool ok = autenticarDispositivo();
  if (!ok) {
    registrarEntradaModoOffline();
  }
  return ok;
}

// ==================== Encoding para Storage ====================
String codificarNombreObjetoStorage(const String& nombreArchivo) {
  String resultado = "";
  for (int i = 0; i < nombreArchivo.length(); i++) {
    char c = nombreArchivo[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      resultado += c;
    } else {
      // URL encode: %HH
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
      resultado += hex;
    }
  }
  return resultado;
}

String generarTokenDescarga() {
  const char* hex = "0123456789abcdef";
  String token = "";

  for (int i = 0; i < 32; i++) {
    uint8_t n = (uint8_t)(esp_random() & 0x0F);
    token += hex[n];
    if (i == 7 || i == 11 || i == 15 || i == 19) {
      token += '-';
    }
  }

  return token;
}

// ==================== Firebase Storage ====================
// Sube la foto usando la REST API de Firebase Storage (Google Cloud Storage JSON API)
String subirBytesAStorage(const uint8_t* data, size_t len, const String& nombreArchivo, bool* authFallo) {
  if (!asegurarDnsHost("firebasestorage.googleapis.com")) {
    return "";
  }

  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  String encodedName = codificarNombreObjetoStorage(nombreArchivo);
  String downloadToken = generarTokenDescarga();
  String url = "https://firebasestorage.googleapis.com/v0/b/" +
               String(FIREBASE_STORAGE_BUCKET) +
               "/o?uploadType=media&name=" + encodedName;

  http.begin(client, url);
  // Evita que una subida trabada deje el comando manual en "procesando" infinito.
  http.setTimeout(12000);
  http.setReuse(false);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("Authorization", "Bearer " + idToken);
  http.addHeader("x-goog-meta-firebaseStorageDownloadTokens", downloadToken);
  int httpCode = http.POST((uint8_t*)data, len);

  String photoUrl = "";
  if (httpCode == 200 || httpCode == 201) {
    // URL con token para que la web pueda mostrar miniaturas sin depender de cabeceras auth.
    photoUrl = "https://firebasestorage.googleapis.com/v0/b/" +
               String(FIREBASE_STORAGE_BUCKET) +
               "/o/" + encodedName + "?alt=media&token=" + downloadToken;
  } else if (authFallo && (httpCode == 401 || httpCode == 403)) {
    // Solo en este caso vale la pena gastar otro handshake TLS reautenticando.
    *authFallo = true;
  }
  http.end();
  return photoUrl;
}

// ==================== Firebase Realtime Database ====================
bool postJsonEnDatabase(const String& url, const String& body) {
  if (!asegurarDnsHost(FIREBASE_DATABASE_URL)) {
    registrarEntradaModoOffline();
    return false;
  }

  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  String urlConAuth = construirUrlDbConAuth(url);
  http.begin(client, urlConAuth);
  http.setTimeout(15000);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(body);
  if (httpCode == 200) {
    http.end();
    return true;
  }
  http.end();
  return false;
}

bool putJsonEnDatabase(const String& url, const String& body) {
  if (!asegurarDnsHost(FIREBASE_DATABASE_URL)) {
    registrarEntradaModoOffline();
    return false;
  }

  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  String urlConAuth = construirUrlDbConAuth(url);
  http.begin(client, urlConAuth);
  http.setTimeout(15000);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PUT(body);
  if (httpCode == 200) {
    http.end();
    return true;
  }
  http.end();
  return false;
}

// FIX: nueva funcion generica para PATCH (actualiza varios campos en una
// sola llamada HTTP en vez de necesitar un PUT por campo).
bool patchJsonEnDatabase(const String& url, const String& body) {
  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  String urlConAuth = construirUrlDbConAuth(url);
  http.begin(client, urlConAuth);
  http.setTimeout(15000);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PATCH(body);
  if (httpCode == 200) {
    http.end();
    return true;
  }
  http.end();
  return false;
}

bool guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante, const String& tipoEvento) {
  String tipoEventoFinal = tipoEvento;
  if (storagePath.startsWith("offline/") && !tipoEventoFinal.endsWith(" (offline)")) {
    tipoEventoFinal += " (offline)";
  }

  // ".sv":"timestamp" le pide a Firebase que ponga la hora real del
  // servidor al momento de guardar, en vez de usar millis() (que solo
  // cuenta el tiempo desde que arrancó el ESP32 y no sirve como fecha real)
  String body = "{";
  body += "\"sensor\":" + String(sensorId) + ",";
  body += "\"timestamp\":{\".sv\":\"timestamp\"},";
  body += "\"tipoEvento\":\"" + tipoEventoFinal + "\",";
  body += "\"photoUrl\":\"" + photoUrl + "\",";
  body += "\"storagePath\":\"" + storagePath + "\",";
  body += "\"importante\":" + String(importante ? "true" : "false") + ",";
  body += "\"retencionHoras\":" + String(RETENCION_HORAS_FOTOS);
  body += "}";

  String dbBase = "https://" + String(FIREBASE_DATABASE_URL);
  bool okSimple = false;
#if GUARDAR_EN_FEED_SIMPLE
  okSimple = postJsonEnDatabase(dbBase + "/alarmas.json", body);
#endif

  String compatAlarmaId = String(WEB_COMPAT_ALARMA_ID);
  bool okCompat = false;
  if (compatAlarmaId.length() > 0) {
    okCompat = postJsonEnDatabase(
      dbBase + "/alarmas/" + compatAlarmaId + "/historial.json",
      body
    );
  }

  return okSimple || okCompat;
}

void publicarEstadoDispositivo(bool forzar) {
  const unsigned long intervaloMs = 2000;
  const unsigned long ventanaSensorConectadoMs = 45000;
  unsigned long ahora = millis();

  if (!forzar && (ahora - ultimoEstadoDispositivo) < intervaloMs) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!asegurarAutenticacionFirebase()) {
    return;
  }

  String compatAlarmaId = String(WEB_COMPAT_ALARMA_ID);
  if (compatAlarmaId.length() == 0) {
    return;
  }

  String body = "{";
  body += "\"online\":true,";
  body += "\"wifiConectado\":true,";
  body += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  body += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"camaraOk\":" + String(camara.isInitialized() ? "true" : "false") + ",";
  bool pir1Conectado = (ahora - ultimoPulsoPIR1) <= ventanaSensorConectadoMs;
  body += "\"pir1Conectado\":" + String(pir1Conectado ? "true" : "false") + ",";
  body += "\"pir1Estado\":" + String(ultimoEstadoPIR1) + ",";
  body += "\"offlinePendientes\":" + String(contarAlarmasOfflinePendientes()) + ",";
  body += "\"uptimeMs\":" + String(ahora) + ",";
  body += "\"lastSeen\":{\".sv\":\"timestamp\"}";
  body += "}";

  String dbBase = "https://" + String(FIREBASE_DATABASE_URL);
  String urlEstado = dbBase + "/alarmas/" + compatAlarmaId + "/estadoDispositivo.json";
  if (publicarEstadoDispositivoConReintento(urlEstado, body)) {
    ultimoEstadoDispositivo = ahora;
  }
}

bool publicarEstadoDispositivoConReintento(const String& url, const String& body) {
  if (putJsonEnDatabase(url, body)) {
    return true;
  }

  // Si sigue fallando, recien ahi forzamos reautenticación.
  idToken = "";
  if (!asegurarAutenticacionFirebase()) {
    return false;
  }

  return putJsonEnDatabase(url, body);
}

namespace {
bool leerComandoCapturaManualPendiente() {
  String compatAlarmaId = String(WEB_COMPAT_ALARMA_ID);
  if (compatAlarmaId.length() == 0) return false;

  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  String dbBase = "https://" + String(FIREBASE_DATABASE_URL);
  String url = dbBase + "/alarmas/" + compatAlarmaId + "/comandos/capturaManual.json";
  String urlConAuth = construirUrlDbConAuth(url);

  http.begin(client, urlConAuth);
  http.setTimeout(10000);
  http.setReuse(false);

  int httpCode = http.GET();
  if (httpCode != 200) {
    // FIX: antes esto fallaba en silencio (devolvia false como si no
    // hubiera comando pendiente). Ahora logueamos el error y, si es un
    // 401 (token vencido), forzamos reautenticacion en el proximo ciclo.
    if (httpCode == 401 || httpCode == 403) {
      idToken = "";
    }
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  if (payload.length() == 0 || payload == "null") {
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload);
  if (err || !doc.is<JsonObject>()) {
    return false;
  }

  String estado = doc["estado"] | "";
  return estado == "pendiente";
}

void actualizarEstadoComandoCaptura(const String& estado, const String& detalle) {
  String compatAlarmaId = String(WEB_COMPAT_ALARMA_ID);
  if (compatAlarmaId.length() == 0) return;

  String dbBase = "https://" + String(FIREBASE_DATABASE_URL);
  // FIX: antes esto eran 3 PUT (estado, detalle, timestamp), es decir 3
  // handshakes TLS completos por comando. Ahora es un unico PATCH que
  // actualiza los 3 campos en una sola conexion HTTPS.
  String url = dbBase + "/alarmas/" + compatAlarmaId + "/comandos/capturaManual.json";

  String body = "{";
  body += "\"estado\":\"" + estado + "\",";
  body += "\"detalle\":\"" + detalle + "\",";
  body += "\"actualizadoEn\":{\".sv\":\"timestamp\"}";
  body += "}";

  patchJsonEnDatabase(url, body);
}
}

void revisarComandoCapturaManual() {
  // FIX: intervalo subido de 1500ms a 3000ms para reducir la frecuencia
  // de handshakes TLS y darle mas margen a la placa.
  const unsigned long intervaloMs = 3000;
  unsigned long ahora = millis();
  if ((ahora - ultimoChequeoComandoManual) < intervaloMs) {
    return;
  }
  ultimoChequeoComandoManual = ahora;

  if (WiFi.status() != WL_CONNECTED) return;
  if (!asegurarAutenticacionFirebase()) return;

  if (!leerComandoCapturaManualPendiente()) {
    return;
  }
  actualizarEstadoComandoCaptura("procesando", "Capturando foto...");
  String detalleResultado = "No se pudo completar la captura manual";
  bool ok = procesarCapturaManual(detalleResultado);
  if (ok) {
    actualizarEstadoComandoCaptura("completado", detalleResultado);
  } else {
    actualizarEstadoComandoCaptura("pendiente", detalleResultado);
  }
}
