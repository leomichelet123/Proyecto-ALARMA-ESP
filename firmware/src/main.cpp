/*
 * Proyecto: Alarma ESP32-CAM
 * - 2 sensores PIR (HC-SR501) digitales
 * - Al detectar movimiento: saca foto, la sube a Firebase Storage
 *   y guarda un registro en Firebase Realtime Database
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include "config.h"

// Token de sesión del dispositivo (se obtiene al autenticarse contra Firebase)
String idToken = "";

unsigned long ultimaAlarmaPIR1 = 0;
unsigned long ultimoHeartbeat = 0;
unsigned long ultimaAlarmaTest = 0;
unsigned long ultimoEstadoDispositivo = 0;
unsigned long ultimoDiagnosticoDb = 0;
unsigned long ultimoChequeoComandoManual = 0;
bool camaraInicializadaOk = false;
bool camaraActiva = false;
int ultimoEstadoPIR1 = LOW;
int estadoAnteriorPIR1 = LOW;
unsigned long ultimoPulsoPIR1 = 0;
unsigned long inicioLowPIR1 = 0;
unsigned long inicioHighPIR1 = 0;
bool sensorPIR1Armado = true;
unsigned long ultimoChequeoSincronizacionOffline = 0;
bool modoOfflineActivo = false;
bool wifiDisponible = false;
bool wifiConectadoPrevio = false;
unsigned long ultimoIntentoReconexionWiFi = 0;
unsigned long ultimoIntentoAuthFirebase = 0;

const unsigned long INTERVALO_RECONEXION_WIFI_MS = 5000;
const unsigned long INTERVALO_REINTENTO_AUTH_MS = 4000;

const char* OFFLINE_QUEUE_FILE = "/offline_queue.txt";
const int MAX_ALARMAS_OFFLINE = 2;
const int MAX_FOTOS_POR_ALARMA_OFFLINE = 1;

// ---------- Prototipos ----------
void conectarWiFi();
bool autenticarDispositivo();
bool asegurarAutenticacionFirebase();
bool inicializarCamara();
camera_fb_t* tomarFoto();
String subirFotoAStorage(camera_fb_t* fb, const String& storagePath);
String subirBytesAStorage(const uint8_t* data, size_t len, const String& storagePath);
void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante, const String& tipoEvento = "movimiento");
void procesarAlarma(int sensorId, bool disparoPorSensor3v = false);
bool procesarCapturaManual(String& detalleResultado);
void revisarComandoCapturaManual();
bool leerComandoCapturaManualPendiente();
void actualizarEstadoComandoCaptura(const String& estado, const String& detalle);
String codificarNombreObjetoStorage(const String& storagePath);
String generarTokenDescarga();
bool postJsonEnDatabase(const String& url, const String& body, const String& etiqueta);
bool putJsonEnDatabase(const String& url, const String& body, const String& etiqueta);
void publicarEstadoDispositivo(bool forzar);
String construirUrlDbConAuth(const String& urlBase);
void diagnosticarEndpointDatabase();
void diagnosticarHostHttps(const char* host, const char* etiqueta);
bool publicarEstadoDispositivoConReintento(const String& url, const String& body);
void configurarClienteSeguro(WiFiClientSecure& client);
void probarHowsMySSL();
void esperarConMillis(unsigned long tiempoMs);
bool asegurarDnsHost(const char* host, const char* etiqueta, int maxIntentos = 3);
void encenderFlashManual();
void apagarFlashManual();
bool inicializarAlmacenamientoOffline();
int contarAlarmasOfflinePendientes();
String generarIdEventoOffline(int sensorId, const String& tipoEvento);
bool escribirArchivoSPIFFS(const String& ruta, const uint8_t* datos, size_t longitud);
bool guardarMetaEventoOffline(const String& eventId, int sensorId, bool importante, const String& tipoEvento);
bool agregarEventoAColaOffline(const String& eventId);
bool obtenerPrimerEventoColaOffline(String& eventId);
bool eliminarPrimerEventoColaOffline();
bool guardarAlarmaOffline(int sensorId, bool importante, const String& tipoEvento, bool usarFlash);
bool sincronizarColaOffline();
bool sincronizarEventoOffline(const String& eventId);
bool leerMetaEventoOffline(const String& eventId, DynamicJsonDocument& meta);
String rutaMetaOffline(const String& eventId);
String rutaFotoOffline(const String& eventId, int indiceFoto);
void registrarEntradaModoOffline(const char* motivo);
void registrarSalidaModoOffline();
void reintentarWiFiEnSegundoPlano();
void manejarEventoWiFi(WiFiEvent_t event, WiFiEventInfo_t info);

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Iniciando sistema de alarma ===");
  Serial.println("[BOOT] setup() iniciado");

#ifdef DIAGNOSTICO_SERIE_SOLO
  Serial.println("[BOOT] Modo diagnostico serie-only activo");
  Serial.println("[BOOT] Si ves esto, la placa arranco y la UART funciona");
  return;
#endif

  Serial.println("[BOOT] Configurando pines PIR");
  // Modo pulsador: pin en LOW estable y sube a HIGH al aplicar 3.3V.
  pinMode(PIR1_PIN, INPUT_PULLDOWN);
#if FLASH_MANUAL_HABILITADO
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
#endif

  if (!inicializarAlmacenamientoOffline()) {
    Serial.println("[BOOT] No se pudo iniciar SPIFFS. El modo offline quedara deshabilitado.");
  }

  WiFi.onEvent(manejarEventoWiFi);

  Serial.println("[BOOT] Iniciando WiFi");
  conectarWiFi();

  if (wifiDisponible || WiFi.status() == WL_CONNECTED) {
    Serial.println("[BOOT] Autenticando dispositivo en Firebase");
    if (!asegurarAutenticacionFirebase()) {
      Serial.println("[BOOT] No se pudo autenticar en Firebase. El sistema seguira activo, pero no podra subir eventos hasta recuperar autenticacion.");
    }
    diagnosticarEndpointDatabase();
    probarHowsMySSL();
  } else {
    registrarEntradaModoOffline("sin wifi en arranque");
  }

  Serial.println("[BOOT] Iniciando camara");
  if (!inicializarCamara()) {
    Serial.println("ERROR: no se pudo inicializar la cámara. Reiniciando...");
    delay(3000);
    ESP.restart();
  }
  camaraInicializadaOk = true;
  camaraActiva = true;

  // Liberar la camara en reposo deja más RAM interna para TLS/Firebase.
  esp_camera_deinit();
  camaraActiva = false;

  Serial.println("[BOOT] Camara inicializada");
  Serial.println("Sistema listo. Esperando detecciones...");
  publicarEstadoDispositivo(true);
}

void loop() {
  unsigned long ahora = millis();

#ifdef DIAGNOSTICO_SERIE_SOLO
  if (ahora - ultimoHeartbeat >= 1000) {
    ultimoHeartbeat = ahora;
    Serial.printf("[DIAG] serie ok | uptime=%lu ms\n", ahora);
  }
  yield();
  return;
#endif

  if (ahora - ultimoHeartbeat >= 5000) {
    ultimoHeartbeat = ahora;
    Serial.printf("[LOOP] vivo | wifi=%d | uptime=%lu ms\n", WiFi.status(), ahora);
  }

#if MODO_TEST_FOTOS_AUTOMATICO
  if ((ahora - ultimaAlarmaTest) > INTERVALO_TEST_FOTO_MS) {
    ultimaAlarmaTest = ahora;
    Serial.println("[TEST] Disparo automatico de foto");
    procesarAlarma(99, true);
  }
#endif

  bool enlaceCaido = (!wifiDisponible && WiFi.status() != WL_CONNECTED);
  bool conectadoAhora = !enlaceCaido;

  if (conectadoAhora != wifiConectadoPrevio) {
    wifiConectadoPrevio = conectadoAhora;
    if (conectadoAhora) {
      Serial.println("[WIFI] Enlace recuperado");
      // Fuerza publish y primer sync apenas vuelve la red.
      ultimoEstadoDispositivo = 0;
      ultimoChequeoSincronizacionOffline = 0;
    } else {
      Serial.println("[WIFI] Enlace perdido");
      idToken = "";
    }
  }

  if (enlaceCaido) {
    registrarEntradaModoOffline("sin wifi");
    reintentarWiFiEnSegundoPlano();
  } else {
    registrarSalidaModoOffline();
  }

  int estadoPIR1 = PIR1_HABILITADO ? digitalRead(PIR1_PIN) : LOW;
  unsigned long ahoraPulso = millis();

  if (estadoPIR1 != estadoAnteriorPIR1) {
    Serial.printf("[PIR1] Cambio de estado -> %d | armado=%d | wifi=%d\n",
                  estadoPIR1,
                  sensorPIR1Armado ? 1 : 0,
                  WiFi.status());
  }

  bool disparoOfflinePorFlanco = enlaceCaido &&
                                 sensorPIR1Armado &&
                                 estadoAnteriorPIR1 == LOW &&
                                 estadoPIR1 == HIGH &&
                                 (ahora - ultimaAlarmaPIR1) > TIEMPO_ENTRE_ALARMAS_MS;

  if (disparoOfflinePorFlanco) {
    ultimaAlarmaPIR1 = ahora;
    sensorPIR1Armado = false;
    inicioHighPIR1 = 0;
    ultimoPulsoPIR1 = ahoraPulso;
    Serial.println(">>> Movimiento detectado offline: Sensor 1");
    procesarAlarma(1, true);
  }

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
    if (inicioHighPIR1 == 0) {
      inicioHighPIR1 = ahoraPulso;
    }
  } else {
    inicioHighPIR1 = 0;
    if (inicioLowPIR1 == 0) {
      inicioLowPIR1 = ahoraPulso;
    }
    if (!sensorPIR1Armado && (ahoraPulso - inicioLowPIR1) >= TIEMPO_LIBERACION_SENSOR_MS) {
      sensorPIR1Armado = true;
    }
  }

  bool highConfirmado = (estadoPIR1 == HIGH) && (inicioHighPIR1 > 0) &&
                        ((ahoraPulso - inicioHighPIR1) >= TIEMPO_CONFIRMACION_HIGH_MS);

  if (sensorPIR1Armado && highConfirmado && estadoAnteriorPIR1 != HIGH) {
    Serial.printf("[PIR1] HIGH confirmado durante %lu ms\n", ahoraPulso - inicioHighPIR1);
  }

  if (!enlaceCaido && sensorPIR1Armado && highConfirmado && (ahora - ultimaAlarmaPIR1) > TIEMPO_ENTRE_ALARMAS_MS) {
    ultimaAlarmaPIR1 = ahora;
    sensorPIR1Armado = false;
    inicioHighPIR1 = 0;
    Serial.println(">>> Movimiento detectado: Sensor 1");
    procesarAlarma(1, true);
  }

  revisarComandoCapturaManual();
  if (WiFi.status() == WL_CONNECTED && (ahora - ultimoChequeoSincronizacionOffline) >= 10000) {
    ultimoChequeoSincronizacionOffline = ahora;
    sincronizarColaOffline();
  }
  publicarEstadoDispositivo(false);

  estadoAnteriorPIR1 = estadoPIR1;

  yield();
}

// ==================== WiFi ====================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);

  Serial.printf("[WIFI] SSID objetivo: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WIFI] Conexion iniciada en segundo plano");
}

void reintentarWiFiEnSegundoPlano() {
  unsigned long ahora = millis();
  if ((ahora - ultimoIntentoReconexionWiFi) < INTERVALO_RECONEXION_WIFI_MS) {
    return;
  }

  ultimoIntentoReconexionWiFi = ahora;
  Serial.println("[WIFI] Reintento de conexion en segundo plano...");
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void manejarEventoWiFi(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiDisponible = true;
      Serial.println("[WIFI] Evento: conectado con IP");
      registrarSalidaModoOffline();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiDisponible = false;
      idToken = "";
      Serial.println("[WIFI] Evento: desconectado");
      registrarEntradaModoOffline("evento desconexion wifi");
      break;
    default:
      break;
  }
}

// ==================== Autenticación del dispositivo ====================
// Usa la cuenta creada en Authentication para obtener un token que permite
// escribir en la base de datos y en Storage, según las reglas de seguridad.
bool autenticarDispositivo() {
  if (!asegurarDnsHost("identitytoolkit.googleapis.com", "AUTH")) {
    Serial.println("[AUTH] DNS no disponible para identitytoolkit.");
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
      Serial.println("Autenticado como dispositivo correctamente.");
      return true;
    }

    Serial.println("No se pudo leer el token de la respuesta.");
    return false;
  }

  Serial.printf("Intento de autenticacion fallido. Código HTTP: %d\n", httpCode);
  if (httpCode > 0) {
    Serial.println(http.getString());
  }
  http.end();
  client.stop();

  return false;
}

bool asegurarAutenticacionFirebase() {
  if (!wifiDisponible && WiFi.status() != WL_CONNECTED) {
    registrarEntradaModoOffline("sin wifi");
    Serial.println("[AUTH] Sin WiFi, no se puede autenticar.");
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

  Serial.println("[AUTH] Solicitando token Firebase...");
  bool ok = autenticarDispositivo();
  if (!ok) {
    registrarEntradaModoOffline("sin autenticacion/firebase");
    Serial.println("[AUTH] Fallo autenticacion Firebase.");
  }
  return ok;
}


bool inicializarCamara() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
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

  if (psramFound()) {
    Serial.println("[BOOT] PSRAM detectada");
    // Modo ultra estable para priorizar TLS/Firebase sobre calidad maxima.
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 16;
    config.fb_count = 1;
  } else {
    Serial.println("[BOOT] PSRAM no detectada");
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 16;
    config.fb_count = 1;
  }

  Serial.println("[BOOT] Llamando esp_camera_init()");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al iniciar la cámara: 0x%x\n", err);
    return false;
  }
  return true;
}

camera_fb_t* tomarFoto() {
  if (!camaraActiva) {
    Serial.println("[CAM] Activando camara para captura");
    if (!inicializarCamara()) {
      Serial.println("[CAM] No se pudo activar la camara");
      camaraInicializadaOk = false;
      return nullptr;
    }
    camaraActiva = true;
    camaraInicializadaOk = true;
  }

  Serial.println("[CAM] Capturando foto");
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error al capturar la foto.");
  } else {
    Serial.printf("[CAM] Foto capturada: %u bytes\n", fb->len);
  }
  return fb;
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
String subirBytesAStorage(const uint8_t* data, size_t len, const String& nombreArchivo) {
  if (!asegurarDnsHost("firebasestorage.googleapis.com", "STORAGE")) {
    Serial.println("[STORAGE] DNS no disponible para Storage.");
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
  http.addHeader("Authorization", "Firebase " + idToken);
  http.addHeader("x-goog-meta-firebaseStorageDownloadTokens", downloadToken);

  Serial.printf("[STORAGE] Subiendo %u bytes a %s\n", (unsigned)len, nombreArchivo.c_str());
  int httpCode = http.POST((uint8_t*)data, len);

  String photoUrl = "";
  if (httpCode == 200) {
    Serial.println("Foto subida correctamente.");
    // URL con token para que la web pueda mostrar miniaturas sin depender de cabeceras auth.
    photoUrl = "https://firebasestorage.googleapis.com/v0/b/" +
               String(FIREBASE_STORAGE_BUCKET) +
               "/o/" + encodedName + "?alt=media&token=" + downloadToken;
    Serial.println("[URL] " + photoUrl);
  } else {
    Serial.printf("Error al subir la foto. Código HTTP: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return photoUrl;
}

String subirFotoAStorage(camera_fb_t* fb, const String& nombreArchivo) {
  if (!fb || !fb->buf || fb->len == 0) {
    return "";
  }
  return subirBytesAStorage(fb->buf, fb->len, nombreArchivo);
}

// ==================== Firebase Realtime Database ====================
bool postJsonEnDatabase(const String& url, const String& body, const String& etiqueta) {
  if (!asegurarDnsHost(FIREBASE_DATABASE_URL, "RTDB")) {
    registrarEntradaModoOffline("sin dns/internet");
    Serial.println(etiqueta + " ERROR");
    Serial.println("Error HTTP: -1");
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
    Serial.println(etiqueta + " OK");
    http.end();
    return true;
  }

  Serial.println(etiqueta + " ERROR");
  Serial.printf("Error HTTP: %d\n", httpCode);
  Serial.println(http.getString());
  http.end();
  return false;
}

bool putJsonEnDatabase(const String& url, const String& body, const String& etiqueta) {
  if (!asegurarDnsHost(FIREBASE_DATABASE_URL, "RTDB")) {
    registrarEntradaModoOffline("sin dns/internet");
    Serial.println(etiqueta + " ERROR");
    Serial.println("Error HTTP: -1");
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
    Serial.println(etiqueta + " OK");
    http.end();
    return true;
  }

  Serial.println(etiqueta + " ERROR");
  Serial.printf("Error HTTP: %d\n", httpCode);
  Serial.println(http.getString());
  http.end();
  return false;
}

void publicarEstadoDispositivo(bool forzar) {
  const unsigned long intervaloMs = 5000;
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
  body += "\"camaraOk\":" + String(camaraInicializadaOk ? "true" : "false") + ",";
  bool pir1Conectado = (ahora - ultimoPulsoPIR1) <= ventanaSensorConectadoMs;
  body += "\"pir1Conectado\":" + String(pir1Conectado ? "true" : "false") + ",";
  body += "\"pir1Estado\":" + String(ultimoEstadoPIR1) + ",";
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
  if (putJsonEnDatabase(url, body, "[DB] estado dispositivo")) {
    return true;
  }

  // Si sigue fallando, recien ahi forzamos reautenticación.
  idToken = "";
  if (!asegurarAutenticacionFirebase()) {
    Serial.println("[DB] estado dispositivo reauth ERROR");
    return false;
  }

  return putJsonEnDatabase(url, body, "[DB] estado dispositivo");
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

void diagnosticarEndpointDatabase() {
  IPAddress ip;
  bool dnsOk = WiFi.hostByName(FIREBASE_DATABASE_URL, ip);
  if (dnsOk) {
    Serial.printf("[DB] DNS OK %s -> %s\n", FIREBASE_DATABASE_URL, ip.toString().c_str());
  } else {
    Serial.printf("[DB] DNS ERROR para %s\n", FIREBASE_DATABASE_URL);
    return;
  }

  WiFiClientSecure testClient;
  configurarClienteSeguro(testClient);
  if (testClient.connect(FIREBASE_DATABASE_URL, 443)) {
    Serial.println("[DB] TLS OK (puerto 443 accesible)");
    testClient.stop();
  } else {
    Serial.println("[DB] TLS ERROR (no se pudo abrir conexion HTTPS con RTDB)");
  }

  diagnosticarHostHttps("identitytoolkit.googleapis.com", "AUTH");
  diagnosticarHostHttps(FIREBASE_DATABASE_URL, "RTDB");
}

void diagnosticarHostHttps(const char* host, const char* etiqueta) {
  WiFiClientSecure testClient;
  configurarClienteSeguro(testClient);

  if (testClient.connect(host, 443)) {
    Serial.printf("[NET] %s HTTPS OK -> %s\n", etiqueta, host);
    testClient.stop();
  } else {
    Serial.printf("[NET] %s HTTPS ERROR -> %s\n", etiqueta, host);
  }
}

void configurarClienteSeguro(WiFiClientSecure& client) {
  client.setInsecure();
  client.setTimeout(15000);
}

void probarHowsMySSL() {
  Serial.println("[TLS] Probando howsmyssl.com...");

  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  const String url = "https://www.howsmyssl.com/a/check";
  if (!http.begin(client, url)) {
    Serial.println("[TLS] howsmyssl begin() ERROR");
    return;
  }

  http.setTimeout(12000);
  http.setReuse(false);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[TLS] howsmyssl HTTP ERROR: %d\n", httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("[TLS] howsmyssl JSON ERROR");
    return;
  }

  const char* tlsVersion = doc["tls_version"] | "desconocida";
  const char* rating = doc["rating"] | "desconocido";

  Serial.printf("[TLS] howsmyssl OK | tls_version=%s | rating=%s\n", tlsVersion, rating);
}

void esperarConMillis(unsigned long tiempoMs) {
  unsigned long inicio = millis();
  while ((millis() - inicio) < tiempoMs) {
    // Mantiene viva la tarea de red sin bloquear con delay().
    yield();
  }
}

void encenderFlashManual() {
#if FLASH_MANUAL_HABILITADO
  digitalWrite(FLASH_LED_PIN, HIGH);
  if (FLASH_PRECAP_MS > 0) {
    esperarConMillis(FLASH_PRECAP_MS);
  }
#endif
}

void apagarFlashManual() {
#if FLASH_MANUAL_HABILITADO
  digitalWrite(FLASH_LED_PIN, LOW);
#endif
}

bool asegurarDnsHost(const char* host, const char* etiqueta, int maxIntentos) {
  if (!wifiDisponible && WiFi.status() != WL_CONNECTED) {
    return false;
  }

  for (int intento = 1; intento <= maxIntentos; intento++) {
    IPAddress ip;
    if (WiFi.hostByName(host, ip)) {
      return true;
    }

    Serial.printf("[DNS] %s intento %d/%d fallido para %s\n", etiqueta, intento, maxIntentos, host);
  }

  return false;
}

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

String rutaMetaOffline(const String& eventId) {
  return "/off_" + eventId + "_meta.json";
}

String rutaFotoOffline(const String& eventId, int indiceFoto) {
  return "/off_" + eventId + "_p" + String(indiceFoto) + ".jpg";
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
  String rutaMeta = rutaMetaOffline(eventId);
  DynamicJsonDocument doc(256);
  doc["eventId"] = eventId;
  doc["sensorId"] = sensorId;
  doc["importante"] = importante;
  doc["tipoEvento"] = tipoEvento;
  doc["fotos"] = MAX_FOTOS_POR_ALARMA_OFFLINE;
  doc["createdAtMs"] = millis();

  File meta = SPIFFS.open(rutaMeta, FILE_WRITE);
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

  String restante = "";
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

  String eventId = generarIdEventoOffline(sensorId, tipoEvento);

  if (!guardarMetaEventoOffline(eventId, sensorId, importante, tipoEvento)) {
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
      return false;
    }

    String rutaFoto = rutaFotoOffline(eventId, i);
    bool ok = escribirArchivoSPIFFS(rutaFoto, fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (!ok) {
      Serial.println("[OFFLINE] No se pudo escribir la foto en SPIFFS.");
      return false;
    }
  }

  if (usarFlash) {
    apagarFlashManual();
  }

  if (!agregarEventoAColaOffline(eventId)) {
    Serial.println("[OFFLINE] No se pudo agregar el evento a la cola.");
    return false;
  }

  Serial.printf("[OFFLINE] Evento guardado localmente: %s\n", eventId.c_str());
  return true;
}

bool leerMetaEventoOffline(const String& eventId, DynamicJsonDocument& meta) {
  String rutaMeta = rutaMetaOffline(eventId);
  File archivo = SPIFFS.open(rutaMeta, FILE_READ);
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
  if (!wifiDisponible && WiFi.status() != WL_CONNECTED) {
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

void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante, const String& tipoEvento) {
  // ".sv":"timestamp" le pide a Firebase que ponga la hora real del
  // servidor al momento de guardar, en vez de usar millis() (que solo
  // cuenta el tiempo desde que arrancó el ESP32 y no sirve como fecha real)
  String body = "{";
  body += "\"sensor\":" + String(sensorId) + ",";
  body += "\"timestamp\":{\".sv\":\"timestamp\"},";
  body += "\"tipoEvento\":\"" + tipoEvento + "\",";
  body += "\"photoUrl\":\"" + photoUrl + "\",";
  body += "\"storagePath\":\"" + storagePath + "\",";
  body += "\"importante\":" + String(importante ? "true" : "false") + ",";
  body += "\"retencionHoras\":" + String(RETENCION_HORAS_FOTOS);
  body += "}";

  String dbBase = "https://" + String(FIREBASE_DATABASE_URL);
  bool okSimple = false;
#if GUARDAR_EN_FEED_SIMPLE
  okSimple = postJsonEnDatabase(dbBase + "/alarmas.json", body, "[DB] alarmas");
#endif

  String compatAlarmaId = String(WEB_COMPAT_ALARMA_ID);
  bool okCompat = false;
  if (compatAlarmaId.length() > 0) {
    okCompat = postJsonEnDatabase(
      dbBase + "/alarmas/" + compatAlarmaId + "/historial.json",
      body,
      "[DB] historial web"
    );
  }

  if (okSimple || okCompat) {
    Serial.println("Alarma registrada en la base de datos.");
  }
}

// ==================== Lógica principal de alarma ====================
void procesarAlarma(int sensorId, bool disparoPorSensor3v) {
  bool usarFlash = disparoPorSensor3v;
  if (!wifiDisponible && WiFi.status() != WL_CONNECTED) {
    registrarEntradaModoOffline("sin wifi");
    Serial.println("[ALARM] Sin WiFi. Guardando alarma localmente para sincronizar luego.");
    guardarAlarmaOffline(sensorId, false, "movimiento", usarFlash);
    return;
  }

  if (!asegurarAutenticacionFirebase()) {
    registrarEntradaModoOffline("sin token firebase");
    Serial.println("[ALARM] Sin token Firebase valido. Guardando alarma localmente para sincronizar luego.");
    guardarAlarmaOffline(sensorId, false, "movimiento", usarFlash);
    return;
  }

  // WiFi conectado no siempre implica salida real a internet.
  // Si no hay DNS para RTDB/Storage, tratamos el evento como offline.
  bool dnsRtdbOk = asegurarDnsHost(FIREBASE_DATABASE_URL, "RTDB", 1);
  bool dnsStorageOk = asegurarDnsHost("firebasestorage.googleapis.com", "STORAGE", 1);
  if (!dnsRtdbOk || !dnsStorageOk) {
    registrarEntradaModoOffline("sin dns/internet");
    Serial.println("[ALARM] Sin salida a internet (DNS), guardando alarma localmente.");
    guardarAlarmaOffline(sensorId, false, "movimiento", usarFlash);
    return;
  }

  registrarSalidaModoOffline();

  Serial.printf("[ALARM] Secuencia iniciada | sensor=%d | fotos=%d\n", sensorId, SECUENCIA_FOTOS_POR_EVENTO);

  for (int i = 0; i < SECUENCIA_FOTOS_POR_EVENTO; i++) {
    if (usarFlash) {
      encenderFlashManual();
    }
    camera_fb_t* fb = tomarFoto();
    if (usarFlash) {
      apagarFlashManual();
    }
    if (!fb) {
      Serial.printf("[ALARM] Foto %d/%d fallida al capturar\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
      String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + "_f" + String(i + 1) + ".jpg";
      guardarAlarmaEnDatabase(sensorId, "", storagePath, false, "movimiento");
      Serial.printf("[ALARM] Foto %d/%d: evento guardado sin foto\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
    } else {
      String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + "_f" + String(i + 1) + ".jpg";
      size_t lenFoto = fb->len;
      uint8_t* copiaFoto = (uint8_t*)malloc(lenFoto);
      if (!copiaFoto) {
        Serial.println("[ALARM] Sin memoria para copiar foto de movimiento.");
        esp_camera_fb_return(fb);
        guardarAlarmaEnDatabase(sensorId, "", storagePath, false, "movimiento");
        Serial.printf("[ALARM] Foto %d/%d: evento guardado sin foto\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
        continue;
      }

      memcpy(copiaFoto, fb->buf, lenFoto);
      esp_camera_fb_return(fb);

      // Igual que captura manual: liberar camara antes de TLS mejora estabilidad de upload.
      if (camaraActiva) {
        esp_camera_deinit();
        camaraActiva = false;
      }

      String photoUrl = "";
      const int maxReintentosSubida = 3;
      for (int intentoSubida = 1; intentoSubida <= maxReintentosSubida; intentoSubida++) {
        photoUrl = subirBytesAStorage(copiaFoto, lenFoto, storagePath);
        if (photoUrl != "") {
          break;
        }
        Serial.printf("[ALARM] Reintento subida %d/%d para foto %d/%d\n",
                      intentoSubida,
                      maxReintentosSubida,
                      i + 1,
                      SECUENCIA_FOTOS_POR_EVENTO);
        if (intentoSubida < maxReintentosSubida) {
          idToken = "";
          asegurarAutenticacionFirebase();
          esperarConMillis(180);
        }
      }
      free(copiaFoto);

      if (photoUrl != "") {
        guardarAlarmaEnDatabase(sensorId, photoUrl, storagePath, false, "movimiento");
        Serial.printf("[ALARM] Foto %d/%d subida y guardada\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
      } else {
        Serial.printf("[ALARM] Foto %d/%d no se pudo subir\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
        guardarAlarmaEnDatabase(sensorId, "", storagePath, false, "movimiento");
        Serial.printf("[ALARM] Foto %d/%d: evento guardado sin foto\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
      }
    }

    if (i < SECUENCIA_FOTOS_POR_EVENTO - 1) {
      esperarConMillis(INTERVALO_ENTRE_FOTOS_MS);
    }
  }

  Serial.println("[ALARM] Secuencia finalizada");

  if (camaraActiva) {
    esp_camera_deinit();
    camaraActiva = false;
    Serial.println("[CAM] Camara liberada tras secuencia");
  }
}

bool procesarCapturaManual(String& detalleResultado) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MANUAL] Sin WiFi, no se puede sacar foto manual.");
    detalleResultado = "Sin WiFi en dispositivo, reintentando";
    return false;
  }

  if (!asegurarAutenticacionFirebase()) {
    Serial.println("[MANUAL] Sin token Firebase valido.");
    detalleResultado = "Sin autenticacion Firebase valida, reintentando";
    return false;
  }

  Serial.println("[MANUAL] Captura manual solicitada desde dashboard.");

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
      if (camaraActiva) {
        esp_camera_deinit();
        camaraActiva = false;
        Serial.println("[CAM] Camara liberada antes de subir captura manual");
      }

      String storagePath = "capturas/manual_" + String(millis()) + "_i1.jpg";
      for (int intentoSubida = 1; intentoSubida <= maxIntentosSubida; intentoSubida++) {
        Serial.printf("[MANUAL] Intento %d/%d de subida manual\n", intentoSubida, maxIntentosSubida);
        String photoUrl = subirBytesAStorage(copiaFoto, lenFoto, storagePath);
        if (photoUrl != "") {
          guardarAlarmaEnDatabase(0, photoUrl, storagePath, true, "captura_manual");
          Serial.println("[MANUAL] Foto manual subida y guardada.");
          fotoSubidaOk = true;
          detalleResultado = "Captura manual registrada en historial";
          break;
        }

        Serial.println("[MANUAL] No se pudo subir la foto manual en este intento.");
        if (intentoSubida < maxIntentosSubida) {
          idToken = "";
          asegurarAutenticacionFirebase();
          esperarConMillis(180);
        }
      }

      free(copiaFoto);
    } else {
      Serial.println("[MANUAL] Sin memoria para copiar la foto manual.");
      esp_camera_fb_return(fb);
    }
  } else {
    Serial.println("[MANUAL] No se pudo capturar la foto manual.");
  }

  if (!fotoSubidaOk) {
    String fallbackPath = "capturas/manual_" + String(millis()) + "_fallback.jpg";
    guardarAlarmaEnDatabase(0, "", fallbackPath, true, "captura_manual");
    Serial.println("[MANUAL] Captura manual registrada sin foto tras reintentos.");
    detalleResultado = "Captura manual registrada sin foto (reintentada)";
  }

  if (camaraActiva) {
    esp_camera_deinit();
    camaraActiva = false;
    Serial.println("[CAM] Camara liberada tras captura manual");
  }

  apagarFlashManual();

  return true;
}

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
  String base = dbBase + "/alarmas/" + compatAlarmaId + "/comandos/capturaManual";

  putJsonEnDatabase(base + "/estado.json", "\"" + estado + "\"", "[CMD] estado");
  putJsonEnDatabase(base + "/detalle.json", "\"" + detalle + "\"", "[CMD] detalle");
  putJsonEnDatabase(base + "/actualizadoEn.json", "{\".sv\":\"timestamp\"}", "[CMD] timestamp");
}

void revisarComandoCapturaManual() {
  const unsigned long intervaloMs = 300;
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

  Serial.println("[CMD] Comando de captura manual detectado.");
  actualizarEstadoComandoCaptura("procesando", "Capturando foto...");
  String detalleResultado = "No se pudo completar la captura manual";
  bool ok = procesarCapturaManual(detalleResultado);
  if (ok) {
    actualizarEstadoComandoCaptura("completado", detalleResultado);
  } else {
    actualizarEstadoComandoCaptura("pendiente", detalleResultado);
  }
}