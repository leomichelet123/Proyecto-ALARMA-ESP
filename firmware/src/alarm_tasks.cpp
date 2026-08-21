#include "alarm_tasks.h"

#include <Arduino.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "config.h"

struct EventoSensor {
  int sensorId;
  bool disparoPorSensor3v;
};

extern bool wifiDisponible;
extern volatile bool capturaOfflineEnCurso;
extern bool procesarAlarma(int sensorId, bool disparoPorSensor3v);
extern void conectarWiFi();
extern void reintentarWiFiEnSegundoPlano();
extern void registrarEntradaModoOffline(const char* motivo);
extern void registrarSalidaModoOffline();
extern void revisarComandoCapturaManual();
extern bool sincronizarColaOffline();
extern void publicarEstadoDispositivo(bool forzar);
extern int contarAlarmasOfflinePendientes();

namespace {
constexpr int TAM_COLA_EVENTOS_SENSOR = 4;
constexpr unsigned long INTERVALO_TAREA_RED_MS = 20;

QueueHandle_t colaEventosSensor = nullptr;
TaskHandle_t tareaRedHandle = nullptr;
TaskHandle_t tareaCapturaHandle = nullptr;

void tareaCaptura(void*) {
  for (;;) {
    EventoSensor evento;
    if (xQueueReceive(colaEventosSensor, &evento, portMAX_DELAY) == pdPASS) {
      Serial.printf("[CAPTURA] Evento %d: procesando captura segun estado de red.\n", evento.sensorId);
      capturaOfflineEnCurso = true;
      bool resultado = procesarAlarma(evento.sensorId, evento.disparoPorSensor3v);
      capturaOfflineEnCurso = false;
      Serial.printf("[CAPTURA] Captura finalizada: %s | Pendientes offline=%d\n",
            resultado ? "OK" : "FALLO",
            contarAlarmasOfflinePendientes());
    }
  }
}

void tareaRed(void*) {
  conectarWiFi();

  unsigned long ultimoEstado = 0;
  unsigned long ultimoSync = 0;
  unsigned long ultimoComando = 0;

  for (;;) {
    unsigned long ahora = millis();

    if (!wifiDisponible || WiFi.status() != WL_CONNECTED) {
      registrarEntradaModoOffline("sin wifi");
      reintentarWiFiEnSegundoPlano();
    } else {
      registrarSalidaModoOffline();
    }

    if (ahora - ultimoComando >= 300) {
      ultimoComando = ahora;
      revisarComandoCapturaManual();
    }

    if (!capturaOfflineEnCurso && WiFi.status() == WL_CONNECTED && ahora - ultimoSync >= 10000) {
      ultimoSync = ahora;
      sincronizarColaOffline();
    }

    if (ahora - ultimoEstado >= 5000) {
      ultimoEstado = ahora;
      publicarEstadoDispositivo(false);
    }

    vTaskDelay(pdMS_TO_TICKS(INTERVALO_TAREA_RED_MS));
  }
}
}

void inicializarTareasAlarma() {
  colaEventosSensor = xQueueCreate(TAM_COLA_EVENTOS_SENSOR, sizeof(EventoSensor));
  if (!colaEventosSensor) {
    Serial.println("[BOOT] ERROR: no se pudo crear cola de eventos del sensor");
  }

  xTaskCreatePinnedToCore(
    tareaCaptura,
    "TareaCaptura",
    8192,
    nullptr,
    2,
    &tareaCapturaHandle,
    1
  );

  xTaskCreatePinnedToCore(
    tareaRed,
    "TareaRed",
    12288,
    nullptr,
    1,
    &tareaRedHandle,
    0
  );

  Serial.println("Sistema listo. Sensor en core 1; red en core 0.");
}

void encolarEventoSensor(int sensorId, bool disparoPorSensor3v) {
  if (!colaEventosSensor) {
    Serial.println("[SENSOR] Cola no disponible; evento descartado");
    return;
  }

  EventoSensor evento = {sensorId, disparoPorSensor3v};
  if (xQueueSend(colaEventosSensor, &evento, 0) != pdPASS) {
    Serial.println("[SENSOR] Cola llena; evento descartado");
    return;
  }

  Serial.println("[SENSOR] Evento encolado para tarea de red");
}
