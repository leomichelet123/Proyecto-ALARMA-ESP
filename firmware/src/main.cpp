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

// Evita disparos repetidos del mismo sensor en poco tiempo
#define TIEMPO_ENTRE_ALARMAS_MS 15000

unsigned long ultimaAlarmaPIR1 = 0;
unsigned long ultimaAlarmaPIR2 = 0;
unsigned long ultimoHeartbeat = 0;
unsigned long ultimaAlarmaTest = 0;

// ---------- Prototipos ----------
void conectarWiFi();
bool autenticarDispositivo();
bool inicializarCamara();
camera_fb_t* tomarFoto();
String subirFotoAStorage(camera_fb_t* fb, const String& storagePath);
void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl, const String& storagePath, bool importante);
void procesarAlarma(int sensorId);
String codificarNombreObjetoStorage(const String& storagePath);
bool postJsonEnDatabase(const String& url, const String& body, const String& etiqueta);

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

  Serial.println("[BOOT] Iniciando camara");
  if (!inicializarCamara()) {
    Serial.println("ERROR: no se pudo inicializar la cámara. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("[BOOT] Camara inicializada");
  Serial.println("Sistema listo. Esperando detecciones...");
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
  int estadoPIR2 = digitalRead(PIR2_PIN);

  if (estadoPIR1 == HIGH && (ahora - ultimaAlarmaPIR1) > TIEMPO_ENTRE_ALARMAS_MS) {
    ultimaAlarmaPIR1 = ahora;
    Serial.println(">>> Movimiento detectado: Sensor 1");
    procesarAlarma(1);
  }

  if (estadoPIR2 == HIGH && (ahora - ultimaAlarmaPIR2) > TIEMPO_ENTRE_ALARMAS_MS) {
    ultimaAlarmaPIR2 = ahora;
    Serial.println(">>> Movimiento detectado: Sensor 2");
    procesarAlarma(2);
  }

  delay(200);
}

// ==================== WiFi ====================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando a WiFi");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nNo se pudo conectar al WiFi.");
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
    client.setInsecure();
    client.setTimeout(10000);

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
    config.frame_size = FRAMESIZE_SVGA;   // 800x600, buena calidad sin ser muy pesada
    config.jpeg_quality = 12;             // menor número = mejor calidad
    config.fb_count = 2;
  } else {
    Serial.println("[BOOT] PSRAM no detectada");
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 15;
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
  Serial.println("[CAM] Capturando foto");
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error al capturar la foto.");
  } else {
    Serial.printf("[CAM] Foto capturada: %u bytes\n", fb->len);
  }
  return fb;
}

// ==================== Firebase Storage ====================
// Sube la foto usando la REST API de Firebase Storage (Google Cloud Storage JSON API)
String subirFotoAStorage(camera_fb_t* fb, const String& nombreArchivo) {
  WiFiClientSecure client;
  client.setInsecure(); // simplifica el proyecto; para producción, validar certificado
  client.setTimeout(15000);

  HTTPClient http;
  String encodedName = codificarNombreObjetoStorage(storagePath);
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
  client.setInsecure();

  HTTPClient http;
  String url = "https://" + String(FIREBASE_DATABASE_URL) + "/alarmas.json";

  http.begin(client, url);
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
  camera_fb_t* fb = tomarFoto();
  if (!fb) return;

  String storagePath = "alarmas/sensor" + String(sensorId) + "_" + String(millis()) + ".jpg";
  String photoUrl = subirFotoAStorage(fb, storagePath);

  esp_camera_fb_return(fb); // liberar memoria de la foto

  if (photoUrl != "") {
    guardarAlarmaEnDatabase(sensorId, photoUrl, storagePath, false);
  }
}
