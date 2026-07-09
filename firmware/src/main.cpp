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
#include "esp_camera.h"
#include "camera_pins.h"
#include "config.h"

// Token de sesión del dispositivo (se obtiene al autenticarse contra Firebase)
String idToken = "";

unsigned long ultimaAlarmaPIR1 = 0;
unsigned long ultimaAlarmaPIR2 = 0;
unsigned long ultimoHeartbeat = 0;
unsigned long ultimaAlarmaTest = 0;
unsigned long ultimoEstadoDispositivo = 0;
unsigned long ultimoDiagnosticoDb = 0;
bool camaraInicializadaOk = false;
bool camaraActiva = false;
int ultimoEstadoPIR1 = LOW;
int ultimoEstadoPIR2 = LOW;
unsigned long ultimoPulsoPIR1 = 0;
unsigned long ultimoPulsoPIR2 = 0;

// ---------- Prototipos ----------
void conectarWiFi();
bool autenticarDispositivo();
bool asegurarAutenticacionFirebase();
bool inicializarCamara();
camera_fb_t* tomarFoto();
String subirFotoAStorage(camera_fb_t* fb, const String& storagePath);
void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante);
void procesarAlarma(int sensorId);
String codificarNombreObjetoStorage(const String& storagePath);
bool postJsonEnDatabase(const String& url, const String& body, const String& etiqueta);
bool putJsonEnDatabase(const String& url, const String& body, const String& etiqueta);
void publicarEstadoDispositivo(bool forzar);
String construirUrlDbConAuth(const String& urlBase);
void diagnosticarEndpointDatabase();
void diagnosticarHostHttps(const char* host, const char* etiqueta);
bool publicarEstadoDispositivoConReintento(const String& url, const String& body);
void configurarClienteSeguro(WiFiClientSecure& client);
void probarHowsMySSL();

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Iniciando sistema de alarma ===");
  Serial.println("[BOOT] setup() iniciado");

#ifdef DIAGNOSTICO_SERIE_SOLO
  Serial.println("[BOOT] Modo diagnostico serie-only activo");
  Serial.println("[BOOT] Si ves esto, la placa arranco y la UART funciona");
  return;
#endif

  Serial.println("[BOOT] Configurando pines PIR");
  // Si el sensor no esta conectado, INPUT_PULLDOWN evita lecturas flotantes (HIGH aleatorio).
  pinMode(PIR1_PIN, INPUT_PULLDOWN);
  pinMode(PIR2_PIN, INPUT_PULLDOWN);

  Serial.println("[BOOT] Iniciando WiFi");
  conectarWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[BOOT] Autenticando dispositivo en Firebase");
    if (!asegurarAutenticacionFirebase()) {
      Serial.println("[BOOT] No se pudo autenticar en Firebase. El sistema seguira activo, pero no podra subir eventos hasta recuperar autenticacion.");
    }
    diagnosticarEndpointDatabase();
    probarHowsMySSL();
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
  delay(10);
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
    procesarAlarma(99);
  }
#endif

  // Reconectar WiFi si se cae
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado, reconectando...");
    conectarWiFi();
  }

  int estadoPIR1 = digitalRead(PIR1_PIN);
  int estadoPIR2 = PIR2_HABILITADO ? digitalRead(PIR2_PIN) : LOW;
  ultimoEstadoPIR1 = estadoPIR1;
  ultimoEstadoPIR2 = estadoPIR2;

  unsigned long ahoraPulso = millis();
  if (estadoPIR1 == HIGH) {
    ultimoPulsoPIR1 = ahoraPulso;
  }
  if (estadoPIR2 == HIGH) {
    ultimoPulsoPIR2 = ahoraPulso;
  }

  publicarEstadoDispositivo(false);

  if (estadoPIR1 == HIGH && (ahora - ultimaAlarmaPIR1) > TIEMPO_ENTRE_ALARMAS_MS) {
    ultimaAlarmaPIR1 = ahora;
    Serial.println(">>> Movimiento detectado: Sensor 1");
    procesarAlarma(1);
  }

  if (PIR2_HABILITADO && estadoPIR2 == HIGH && (ahora - ultimaAlarmaPIR2) > TIEMPO_ENTRE_ALARMAS_MS) {
    ultimaAlarmaPIR2 = ahora;
    Serial.println(">>> Movimiento detectado: Sensor 2");
    procesarAlarma(2);
  }

  delay(200);
}

// ==================== WiFi ====================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);

  Serial.printf("[WIFI] SSID objetivo: %s\n", WIFI_SSID);

  int canalObjetivo = 0;
  int redes = WiFi.scanNetworks(false, true);
  if (redes <= 0) {
    Serial.println("[WIFI] No se detectaron redes en el escaneo inicial.");
  } else {
    bool encontrada = false;
    for (int i = 0; i < redes; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid == String(WIFI_SSID)) {
        encontrada = true;
        canalObjetivo = WiFi.channel(i);
        Serial.printf("[WIFI] Red encontrada | canal=%d | RSSI=%d dBm | encript=%d\n",
                      canalObjetivo,
                      WiFi.RSSI(i),
                      (int)WiFi.encryptionType(i));
        break;
      }
    }

    if (!encontrada) {
      Serial.println("[WIFI] El SSID configurado no aparece en el escaneo.");
    }
  }

  if (canalObjetivo > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, canalObjetivo);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  Serial.print("Conectando a WiFi");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 40) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nNo se pudo conectar al WiFi.");
    Serial.printf("[WIFI] Estado final=%d\n", WiFi.status());
    Serial.println("[WIFI] Si aparece 4WAY_HANDSHAKE_TIMEOUT, revisar clave y seguridad WPA2/WPA3 del router/hotspot.");
    Serial.println("[BOOT] Continuando sin WiFi; las subidas a Firebase van a fallar");
  }
}

// ==================== Autenticación del dispositivo ====================
// Usa la cuenta creada en Authentication para obtener un token que permite
// escribir en la base de datos y en Storage, según las reglas de seguridad.
bool autenticarDispositivo() {
  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(FIREBASE_API_KEY);
  String body = "{\"email\":\"" + String(FIREBASE_DEVICE_EMAIL) +
                "\",\"password\":\"" + String(FIREBASE_DEVICE_PASSWORD) +
                "\",\"returnSecureToken\":true}";

  for (int intento = 1; intento <= 3; intento++) {
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
      } else {
        Serial.println("No se pudo leer el token de la respuesta.");
        return false;
      }
    }

    Serial.printf("Intento %d/3 fallido al autenticar. Código HTTP: %d\n", intento, httpCode);
    if (httpCode > 0) {
      Serial.println(http.getString());
    }
    http.end();
    client.stop();

    if (intento < 3) {
      delay(4000);
    }
  }

  return false;
}

bool asegurarAutenticacionFirebase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AUTH] Sin WiFi, no se puede autenticar.");
    return false;
  }

  if (idToken.length() > 0) {
    return true;
  }

  Serial.println("[AUTH] Solicitando token Firebase...");
  bool ok = autenticarDispositivo();
  if (!ok) {
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

// ==================== Firebase Storage ====================
// Sube la foto usando la REST API de Firebase Storage (Google Cloud Storage JSON API)
String subirFotoAStorage(camera_fb_t* fb, const String& nombreArchivo) {
  WiFiClientSecure client;
  configurarClienteSeguro(client);

  HTTPClient http;
  String encodedName = codificarNombreObjetoStorage(nombreArchivo);
  String url = "https://firebasestorage.googleapis.com/v0/b/" +
               String(FIREBASE_STORAGE_BUCKET) +
               "/o?uploadType=media&name=" + encodedName;

  http.begin(client, url);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("Authorization", "Firebase " + idToken);

  int httpCode = http.POST(fb->buf, fb->len);

  String photoUrl = "";
  if (httpCode == 200) {
    Serial.println("Foto subida correctamente.");
    // URL pública de descarga (funciona si las reglas de Storage permiten lectura)
    photoUrl = "https://firebasestorage.googleapis.com/v0/b/" +
               String(FIREBASE_STORAGE_BUCKET) +
               "/o/" + encodedName + "?alt=media";
    Serial.println("[URL] " + photoUrl);
  } else {
    Serial.printf("Error al subir la foto. Código HTTP: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return photoUrl;
}

// ==================== Firebase Realtime Database ====================
bool postJsonEnDatabase(const String& url, const String& body, const String& etiqueta) {
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
  const unsigned long intervaloMs = 15000;
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
  bool pir2Conectado = PIR2_HABILITADO && ((ahora - ultimoPulsoPIR2) <= ventanaSensorConectadoMs);
  body += "\"pir1Conectado\":" + String(pir1Conectado ? "true" : "false") + ",";
  body += "\"pir2Conectado\":" + String(pir2Conectado ? "true" : "false") + ",";
  body += "\"pir1Estado\":" + String(ultimoEstadoPIR1) + ",";
  body += "\"pir2Estado\":" + String(ultimoEstadoPIR2) + ",";
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

  // Reintento simple: evita sobrecargar TLS con reauth agresivo.
  delay(400);
  Serial.println("[DB] estado dispositivo reintento...");
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

void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante) {
  // ".sv":"timestamp" le pide a Firebase que ponga la hora real del
  // servidor al momento de guardar, en vez de usar millis() (que solo
  // cuenta el tiempo desde que arrancó el ESP32 y no sirve como fecha real)
  String body = "{";
  body += "\"sensor\":" + String(sensorId) + ",";
  body += "\"timestamp\":{\".sv\":\"timestamp\"},";
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
void procesarAlarma(int sensorId) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ALARM] Sin WiFi, no se puede procesar la alarma en Firebase.");
    return;
  }

  if (!asegurarAutenticacionFirebase()) {
    Serial.println("[ALARM] Sin token Firebase valido, se cancela el envio de la alarma.");
    return;
  }

  Serial.printf("[ALARM] Secuencia iniciada | sensor=%d | fotos=%d\n", sensorId, SECUENCIA_FOTOS_POR_EVENTO);

  for (int i = 0; i < SECUENCIA_FOTOS_POR_EVENTO; i++) {
    camera_fb_t* fb = tomarFoto();
    if (!fb) {
      Serial.printf("[ALARM] Foto %d/%d fallida al capturar\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
      String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + "_f" + String(i + 1) + ".jpg";
      guardarAlarmaEnDatabase(sensorId, "", storagePath, false);
      Serial.printf("[ALARM] Foto %d/%d: evento guardado sin foto\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
    } else {
      String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + "_f" + String(i + 1) + ".jpg";
      String photoUrl = subirFotoAStorage(fb, storagePath);
      esp_camera_fb_return(fb); // liberar memoria de la foto

      if (photoUrl != "") {
        guardarAlarmaEnDatabase(sensorId, photoUrl, storagePath, false);
        Serial.printf("[ALARM] Foto %d/%d subida y guardada\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
      } else {
        Serial.printf("[ALARM] Foto %d/%d no se pudo subir\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
        guardarAlarmaEnDatabase(sensorId, "", storagePath, false);
        Serial.printf("[ALARM] Foto %d/%d: evento guardado sin foto\n", i + 1, SECUENCIA_FOTOS_POR_EVENTO);
      }
    }

    if (i < SECUENCIA_FOTOS_POR_EVENTO - 1) {
      delay(INTERVALO_ENTRE_FOTOS_MS);
    }
  }

  Serial.println("[ALARM] Secuencia finalizada");

  if (camaraActiva) {
    esp_camera_deinit();
    camaraActiva = false;
    Serial.println("[CAM] Camara liberada tras secuencia");
  }
}
