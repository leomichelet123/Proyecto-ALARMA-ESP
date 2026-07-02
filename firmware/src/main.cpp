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

// ---------- Prototipos ----------
void conectarWiFi();
bool autenticarDispositivo();
bool inicializarCamara();
camera_fb_t* tomarFoto();
String subirFotoAStorage(camera_fb_t* fb, const String& nombreArchivo);
void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl);
void procesarAlarma(int sensorId);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Iniciando sistema de alarma ===");

  pinMode(PIR1_PIN, INPUT);
  pinMode(PIR2_PIN, INPUT);

  conectarWiFi();

  if (autenticarDispositivo()) {
    Serial.println("Credenciales del dispositivo verificadas.");
  } else {
    Serial.println("ADVERTENCIA: no se pudo autenticar el dispositivo. Revisar email/password en config.h");
  }

  if (!inicializarCamara()) {
    Serial.println("ERROR: no se pudo inicializar la cámara. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Sistema listo. Esperando detecciones...");
}

void loop() {
  // Reconectar WiFi si se cae
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado, reconectando...");
    conectarWiFi();
  }

  int estadoPIR1 = digitalRead(PIR1_PIN);
  int estadoPIR2 = digitalRead(PIR2_PIN);

  unsigned long ahora = millis();

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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;   // 800x600, buena calidad sin ser muy pesada
    config.jpeg_quality = 12;             // menor número = mejor calidad
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al iniciar la cámara: 0x%x\n", err);
    return false;
  }
  return true;
}

camera_fb_t* tomarFoto() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error al capturar la foto.");
  }
  return fb;
}

// ==================== Firebase Storage ====================
// Sube la foto usando la REST API de Firebase Storage (Google Cloud Storage JSON API)
String subirFotoAStorage(camera_fb_t* fb, const String& nombreArchivo) {
  Serial.printf("Heap libre antes de subir foto: %u bytes (tamaño foto: %u bytes)\n", ESP.getFreeHeap(), fb->len);

  WiFiClientSecure client;
  client.setInsecure(); // simplifica el proyecto; para producción, validar certificado
  client.setTimeout(15000);

  HTTPClient http;
  String url = "https://firebasestorage.googleapis.com/v0/b/" +
               String(FIREBASE_STORAGE_BUCKET) +
               "/o?uploadType=media&name=" + nombreArchivo;

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
               "/o/" + nombreArchivo + "?alt=media";
  } else {
    Serial.printf("Error al subir la foto. Código HTTP: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return photoUrl;
}

// ==================== Firebase Realtime Database ====================
void guardarAlarmaEnDatabase(int sensorId, const String& photoUrl) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://" + String(FIREBASE_DATABASE_URL) +
               "/alarmas/" + String(ALARMA_ID) + "/historial.json" +
               "?auth=" + idToken;

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  // ".sv":"timestamp" le pide a Firebase que ponga la hora real del
  // servidor al momento de guardar, en vez de usar millis() (que solo
  // cuenta el tiempo desde que arrancó el ESP32 y no sirve como fecha real)
  String body = "{";
  body += "\"sensor\":" + String(sensorId) + ",";
  body += "\"timestamp\":{\".sv\":\"timestamp\"},";
  body += "\"photoUrl\":\"" + photoUrl + "\"";
  body += "}";

  int httpCode = http.POST(body);

  if (httpCode == 200) {
    Serial.println("Alarma registrada en la base de datos.");
  } else {
    Serial.printf("Error al guardar en la base de datos. Código HTTP: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
}

// ==================== Lógica principal de alarma ====================
void procesarAlarma(int sensorId) {
  Serial.printf("Heap libre antes de procesar: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Señal WiFi (RSSI): %d dBm\n", WiFi.RSSI());

  if (!autenticarDispositivo()) {
    Serial.println("No se pudo autenticar, se cancela esta alarma.");
    return;
  }

  camera_fb_t* fb = tomarFoto();
  if (!fb) return;

  String nombreArchivo = "alarmas%2Fsensor" + String(sensorId) + "_" + String(millis()) + ".jpg";
  String photoUrl = subirFotoAStorage(fb, nombreArchivo);

  esp_camera_fb_return(fb); // liberar memoria de la foto

  if (photoUrl != "") {
    guardarAlarmaEnDatabase(sensorId, photoUrl);
  }
}
