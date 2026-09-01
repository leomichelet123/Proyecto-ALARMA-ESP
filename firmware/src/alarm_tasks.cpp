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
extern void registrarEntradaModoOffline();
extern void registrarSalidaModoOffline();
extern void revisarComandoCapturaManual();
extern bool sincronizarColaOffline();
extern void publicarEstadoDispositivo(bool forzar);
extern bool asegurarAutenticacionFirebase();

namespace {
constexpr int TAM_COLA_EVENTOS_SENSOR = 4;
constexpr unsigned long INTERVALO_TAREA_RED_MS = 20;
//se declara una cola de FreeRTOS.
QueueHandle_t colaEventosSensor = nullptr;

void tareaCaptura(void*) {
  for (;;) {
    EventoSensor evento;
    if (xQueueReceive(colaEventosSensor, &evento, portMAX_DELAY) == pdPASS) {
      capturaOfflineEnCurso = true;
      procesarAlarma(evento.sensorId, evento.disparoPorSensor3v);
      capturaOfflineEnCurso = false;
    }
  }
}

void tareaRed(void*) {
  conectarWiFi();

  unsigned long ultimoEstado = 0;
  unsigned long ultimoSync = 0;
  unsigned long ultimoComando = 0;
  bool estabaConectado = false;

  for (;;) {
    unsigned long ahora = millis();
    bool conectado = wifiDisponible && WiFi.status() == WL_CONNECTED;

    if (!conectado) {
      registrarEntradaModoOffline();
      reintentarWiFiEnSegundoPlano();
    } else {
      registrarSalidaModoOffline();
    }

    if (conectado != estabaConectado) {
      estabaConectado = conectado;
      if (conectado) {
        asegurarAutenticacionFirebase();
        publicarEstadoDispositivo(true);
        if (!capturaOfflineEnCurso) {
          sincronizarColaOffline();
        }
        ultimoEstado = ahora;
        ultimoSync = ahora;
      }
    }

    if (ahora - ultimoComando >= 300) {
      ultimoComando = ahora;
      revisarComandoCapturaManual();
    }

    if (!capturaOfflineEnCurso && conectado && ahora - ultimoSync >= 2000) {
      ultimoSync = ahora;
      sincronizarColaOffline();
    }

    if (conectado && ahora - ultimoEstado >= 2000) {
      ultimoEstado = ahora;
      publicarEstadoDispositivo(false);
    }

    vTaskDelay(pdMS_TO_TICKS(INTERVALO_TAREA_RED_MS));
  }
}
}

void inicializarTareasAlarma() {
  colaEventosSensor = xQueueCreate(TAM_COLA_EVENTOS_SENSOR, sizeof(EventoSensor));
//FreeRTOS
  xTaskCreatePinnedToCore(
    tareaCaptura,
    "TareaCaptura",
    8192,
    nullptr,
    2,
    nullptr,
    1
  );

  xTaskCreatePinnedToCore(
    tareaRed,
    "TareaRed",
    12288,
    nullptr,
    1,
    nullptr,
    0
  );
}

void encolarEventoSensor(int sensorId, bool disparoPorSensor3v) {
  if (!colaEventosSensor) {
    return;
  }

  EventoSensor evento = {sensorId, disparoPorSensor3v};
  if (xQueueSend(colaEventosSensor, &evento, 0) != pdPASS) {
    return;
  }
}


