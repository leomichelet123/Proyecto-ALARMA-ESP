# Alarma ESP32-CAM

Sistema de alarma con 2 sensores PIR (HC-SR501) conectados a un ESP32-CAM.
Al detectar movimiento, el ESP32 saca una foto, la sube a Firebase Storage
y registra la alarma en Firebase Realtime Database. Una página web permite
loguearse y ver el histórico de alarmas con sus fotos.

## Estructura del repositorio

```
alarma-esp32-proyecto/
├── firmware/          Código del ESP32-CAM (PlatformIO)
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp
│       ├── camera_pins.h
│       └── config.h.example   <- copiar a config.h y completar
└── web/                Página web (login + histórico)
    └── public/
        ├── index.html         Login
        ├── dashboard.html     Panel con histórico
        ├── css/style.css
        └── js/
```

## Cómo levantar el firmware

1. Abrir la carpeta `firmware/` en VS Code con la extensión PlatformIO
2. Copiar `src/config.h.example` a `src/config.h`
3. Completar `WIFI_SSID` y `WIFI_PASSWORD` en `config.h`
4. Conectar la placa ESP32-CAM-MB por USB, compilar y subir (Build → Upload)

## Cómo levantar la web

La carpeta `web/public` está lista para desplegarse en Firebase Hosting,
o para abrirse localmente con un servidor estático simple.

## Servicios de Firebase usados

- **Authentication**: login con correo/contraseña
- **Realtime Database**: histórico de alarmas (`/alarmas`)
- **Storage**: fotos capturadas en cada alarma
