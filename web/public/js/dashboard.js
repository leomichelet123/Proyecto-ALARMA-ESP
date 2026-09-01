let alarmaId = null;
let miUid = null;
let adminUid = null;
let tieneErrorUsuarios = false;
let tieneErrorHistorial = false;
let ultimoTimestampVisto = null;
let ultimaLimpiezaHuerfanosMs = 0;
let puedeVerificarAuthPorEmail = null;
const RETENCION_DIAS_HISTORIAL = 7;
const RETENCION_MS_HISTORIAL = RETENCION_DIAS_HISTORIAL * 24 * 60 * 60 * 1000;
const LIMPIEZA_RETENCION_INTERVALO_MS = 15 * 60 * 1000;
// El ESP32 escribe su lastSeen cada 2s con WiFi; con 3.5s alcanza para tolerar
// un envio perdido sin tardar hasta 1 minuto en marcar "Sin conexion".
const VENTANA_ONLINE_CAMARA_MS = 3500;
const REFRESCO_SUAVE_MS = 3000;
let ultimaLimpiezaRetencionMs = 0;
let limpiezaRetencionEnCurso = false;
let autoRefreshTimer = null;
let ultimoEstadoDispositivoCache = null;
const WEB_COMPAT_ALARMA_ID = 'alarma001';
let ultimoEstadoComandoManualTs = 0;
const MAX_MS_PENDIENTE_MANUAL = 15000;
const MAX_MS_PROCESANDO_MANUAL = 25000;
let ultimoComandoManual = null;
let serverTimeOffsetMs = 0;

function elegirEstadoMasReciente(a, b) {
  const aTs = Number((a && a.lastSeen) || 0);
  const bTs = Number((b && b.lastSeen) || 0);
  return bTs > aTs ? (b || null) : (a || null);
}

function mergeHistorial(a, b) {
  return Object.assign({}, a || {}, b || {});
}

const connectionStatus = document.getElementById('connection-status');
const syncAlert = document.getElementById('sync-alert');
const alarmBanner = document.getElementById('alarm-banner');
const dotCamera = document.getElementById('dot-camera');
const statusCamera = document.getElementById('status-camera');
const dotPirGeneral = document.getElementById('dot-pir-general');
const statusPirGeneral = document.getElementById('status-pir-general');
const btnPhotoNow = document.getElementById('btn-photo-now');
const manualPhotoStatus = document.getElementById('manual-photo-status');

// ---------- Notificaciones ----------
function cerrarBanner() {
  alarmBanner.classList.remove('visible');
}

function setManualPhotoStatus(msg, tipo = 'normal') {
  if (!manualPhotoStatus) return;
  manualPhotoStatus.textContent = msg;
  manualPhotoStatus.classList.remove('ok', 'error');
  if (tipo === 'ok' || tipo === 'error') {
    manualPhotoStatus.classList.add(tipo);
  }
}

function ahoraServidorMs() {
  return Date.now() + serverTimeOffsetMs;
}

function mostrarBanner() {
  alarmBanner.classList.add('visible');
  // Vibrar si es móvil
  if (navigator.vibrate) navigator.vibrate([400, 100, 400, 100, 400]);
  // Sonido de alarma
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    [0, 0.3, 0.6].forEach((delay) => {
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.frequency.value = 880;
      osc.type = 'square';
      gain.gain.setValueAtTime(0.3, ctx.currentTime + delay);
      gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + delay + 0.25);
      osc.start(ctx.currentTime + delay);
      osc.stop(ctx.currentTime + delay + 0.25);
    });
  } catch (e) { /* navegador no soporta AudioContext */ }
}

async function iniciarNotificacionesPush() {
  if (!('Notification' in window) || !('serviceWorker' in navigator)) return;

  let permiso = Notification.permission;
  if (permiso === 'default') {
    permiso = await Notification.requestPermission();
  }
  if (permiso !== 'granted') return;

  try {
    const registration = await navigator.serviceWorker.register('/firebase-messaging-sw.js');
    const messaging = firebase.messaging();
    // VAPID key pública del proyecto (se obtiene en Firebase Console > Cloud Messaging)
    const token = await messaging.getToken({
      vapidKey: 'BLBz2zQKq0Kj7Hf9Xk3mN8vYpRtWsUeA1iC4dGjOlM5nPqVxZbDwEhFuIsJyTr6',
      serviceWorkerRegistration: registration
    });
    if (token && miUid && alarmaId) {
      await db.ref('alarmas/' + alarmaId + '/fcmTokens/' + miUid).set(token);
    }
    // Notificación cuando la app está en primer plano
    messaging.onMessage(() => {
      mostrarBanner();
    });
  } catch (e) {
    console.warn('FCM no disponible:', e.message);
  }
}

function nombreDesdeUsuario(user) {
  const fromDisplay = (user.displayName || '').trim();
  if (fromDisplay) return fromDisplay;
  const fromEmail = (user.email || '').split('@')[0].replace(/[._-]+/g, ' ').trim();
  return fromEmail || 'Usuario';
}

async function intentarRecuperarAlarmaPorEmail(user) {
  if (!user || !user.email) return null;

  try {
    const alarmasSnap = await db.ref('alarmas').once('value');
    const alarmas = alarmasSnap.val() || {};

  for (const [id, alarma] of Object.entries(alarmas)) {
    const usuarios = (alarma && alarma.usuarios) ? alarma.usuarios : {};
    const yaEstaPorUid = !!usuarios[user.uid];
    let encontradoPorEmail = false;

    for (const u of Object.values(usuarios)) {
      if (u && typeof u === 'object' && (u.email || '').toLowerCase() === user.email.toLowerCase()) {
        encontradoPorEmail = true;
        break;
      }
    }

    if (yaEstaPorUid || encontradoPorEmail) {
      // Reestablece mapeo del usuario actual a su alarma.
      await db.ref('userAlarma/' + user.uid).set(id);

      // Solo crear nodo por UID si no existe por UID NI por email.
      if (!yaEstaPorUid && !encontradoPorEmail) {
        await db.ref('alarmas/' + id + '/usuarios/' + user.uid).set({
          nombre: nombreDesdeUsuario(user),
          email: user.email,
          fechaAlta: firebase.database.ServerValue.TIMESTAMP
        });
      }

      return id;
    }
  }
  } catch (e) {
    console.warn('Recuperación por email no disponible:', e.message);
  }

  return null;
}

// ---------- Protección de ruta + resolver a qué alarma pertenezco ----------
auth.onAuthStateChanged(async (user) => {
  if (!user) {
    if (autoRefreshTimer) {
      clearInterval(autoRefreshTimer);
      autoRefreshTimer = null;
    }
    window.location.href = 'index.html';
    return;
  }

  try {
    miUid = user.uid;
    document.getElementById('user-email').textContent = user.email || '';

    const snap = await db.ref('userAlarma/' + user.uid).once('value');
    alarmaId = snap.val();

    if (!alarmaId) {
      // Intento de autocorreccion: recuperar membresia por email si cambio el UID.
      alarmaId = await intentarRecuperarAlarmaPorEmail(user);
      if (!alarmaId) {
        // Mantener sesion activa y mostrar mensaje en vez de redirigir.
        syncAlert.style.display = 'block';
        syncAlert.innerHTML = 'Tu cuenta no está vinculada a ninguna alarma. <a href="activar.html" style="color:inherit;font-weight:bold;text-decoration:underline;">Ingresá tu código de activación acá</a>.';

        const usersList = document.getElementById('users-list');
        const historyList = document.getElementById('history-list');
        const emptyState = document.getElementById('empty-state');
        usersList.innerHTML = '';
        historyList.innerHTML = '';
        emptyState.style.display = 'block';
        actualizarEstadoDispositivoUI(null);
        return;
      }
    }

    iniciarEscuchas();
  } catch (err) {
    console.error('Error inicializando dashboard:', err);
    syncAlert.style.display = 'block';
    syncAlert.textContent = 'Error: ' + (err.message || err.code || JSON.stringify(err));
    return;
  }
  iniciarNotificacionesPush();
  cargarAdminUid();
  if (!autoRefreshTimer) {
    autoRefreshTimer = setInterval(() => {
      if (document.visibilityState === 'visible') {
        actualizarEstadoDispositivoUI(ultimoEstadoDispositivoCache);
        reevaluarEstadoComandoManual();
        refrescarDashboardSuave();
      }
    }, REFRESCO_SUAVE_MS);
  }
});

// Cargar adminUid cuando alarmaId esté disponible
async function cargarAdminUid() {
  const snap = await db.ref('alarmas/' + alarmaId + '/adminUid').once('value');
  adminUid = snap.val();

  // Si este usuario es admin, dispara una limpieza de retencion al ingresar.
  if (adminUid && miUid === adminUid) {
    try {
      const historialSnap = await db.ref('alarmas/' + alarmaId + '/historial').once('value');
      await limpiarHistorialAntiguoEnSegundoPlano(historialSnap.val() || {}, true);
    } catch (e) {
      console.warn('No se pudo ejecutar limpieza inicial de retencion:', e.message || e);
    }
  }
}

function renderUsuariosLista(data) {
  const usersList = document.getElementById('users-list');
  if (!usersList) return;

  usersList.innerHTML = '';

  const usuarios = Object.entries(data || {}).filter(([uid, u]) => {
    return !!uid && u && typeof u === 'object';
  });

  const total = usuarios.length;
  const allTitles = document.querySelectorAll('.section-title');
  allTitles.forEach(t => {
    if (t.textContent.startsWith('Usuarios')) {
      t.textContent = `Usuarios de esta alarma (${total}/4)`;
    }
  });

  usuarios.forEach(([uid, usuario]) => {
    const nombre = usuario.nombre || usuario.displayName || `Usuario ${uid.slice(0, 6)}`;
    const apellido = usuario.apellido || '';
    const nombreCompleto = apellido ? `${nombre} ${apellido}` : nombre;
    const email = usuario.email || uid;

    const item = document.createElement('div');
    item.className = 'history-item';
    item.style.gridTemplateColumns = '1fr auto';

    const esUnoMismo = uid === miUid;

    item.innerHTML = `
      <div>
        <div class="history-sensor">${nombreCompleto}${esUnoMismo ? ' (vos)' : ''}</div>
        <div class="history-time">${email}</div>
      </div>
    `;

    if (!esUnoMismo && miUid === adminUid) {
      const btnEliminar = document.createElement('button');
      btnEliminar.textContent = 'Eliminar';
      btnEliminar.className = 'link-btn';
      btnEliminar.style.color = 'var(--alert)';
      btnEliminar.addEventListener('click', () => {
        if (confirm(`¿Quitar a ${nombre} de esta alarma?`)) {
          db.ref('alarmas/' + alarmaId + '/usuarios/' + uid).remove();
          db.ref('userAlarma/' + uid).remove();
        }
      });
      item.appendChild(btnEliminar);
    }

    usersList.appendChild(item);
  });
}

function renderHistorialLista(data) {
  const historyList = document.getElementById('history-list');
  const emptyState = document.getElementById('empty-state');
  if (!historyList || !emptyState) return;

  historyList.innerHTML = '';

  if (!data) {
    emptyState.style.display = 'block';
    actualizarEstadoSensorPIR(dotPirGeneral, statusPirGeneral, null);
    return;
  }

  const alarmas = Object.values(data)
    .filter((a) => a && typeof a === 'object' && a.sensor !== undefined)
    .sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));

  if (alarmas.length === 0) {
    emptyState.style.display = 'block';
    actualizarEstadoSensorPIR(dotPirGeneral, statusPirGeneral, null);
    return;
  }

  emptyState.style.display = 'none';

  const masReciente = alarmas[0].timestamp || 0;
  const eventoMasReciente = alarmas[0] || {};
  const esCapturaManualReciente = (eventoMasReciente.tipoEvento === 'captura_manual') || Number(eventoMasReciente.sensor) === 0;
  const ultimoMovimiento = alarmas.find((a) => {
    if (!a || typeof a !== 'object') return false;
    const esCapturaManual = (a.tipoEvento === 'captura_manual') || Number(a.sensor) === 0;
    return !esCapturaManual;
  });
  const masRecienteMovimiento = ultimoMovimiento ? Number(ultimoMovimiento.timestamp || 0) : 0;

  if (ultimoTimestampVisto !== null && masReciente > ultimoTimestampVisto && !esCapturaManualReciente) {
    mostrarBanner();
    if (Notification.permission === 'granted') {
      new Notification('🚨 ALARMA ACTIVADA', {
        body: 'Movimiento detectado — Sensor PIR',
        icon: '/icon-192.png',
        requireInteraction: true
      });
    }
  }
  ultimoTimestampVisto = masReciente;

  actualizarEstadoSensorPIR(dotPirGeneral, statusPirGeneral, masRecienteMovimiento || null);

  alarmas.forEach((alarma) => {
    const item = document.createElement('div');
    item.className = 'history-item';
    const esCapturaManual = (alarma.tipoEvento === 'captura_manual') || Number(alarma.sensor) === 0;
    const tipoEventoRaw = (alarma && typeof alarma.tipoEvento === 'string') ? alarma.tipoEvento.trim() : '';
    const tipoEventoNorm = tipoEventoRaw.toLowerCase();
    const rutaStorage = typeof alarma.storagePath === 'string' ? alarma.storagePath : '';
    const esOffline = tipoEventoNorm.includes('offline') || rutaStorage.startsWith('offline/');
    const tituloEvento = esCapturaManual
      ? 'Captura manual'
      : (esOffline ? 'Sensor de movimiento (offline)' : 'Sensor de movimiento');
    const badgeEvento = esCapturaManual
      ? 'Captura'
      : (esOffline ? 'Foto offline' : 'Movimiento');
    const detalleRuta = esCapturaManual && alarma.storagePath
      ? `<div class="history-time">Guardado en: ${alarma.storagePath}</div>`
      : '';

    const tieneFoto = Boolean(alarma.photoUrl || obtenerPathStorageDesdeEvento(alarma));
    const thumb = tieneFoto ? crearMiniaturaEvento(alarma) : document.createElement('div');
    if (!tieneFoto) {
      thumb.className = 'history-thumb';
    }

    const info = document.createElement('div');
    info.innerHTML = `
      <div class="history-sensor">${tituloEvento}</div>
      <div class="history-time">${formatearFecha(alarma.timestamp)}</div>
      ${detalleRuta}
    `;

    const badge = document.createElement('span');
    badge.className = 'badge';
    badge.textContent = badgeEvento;

    if (tieneFoto) {
      thumb.addEventListener('click', () => {
        abrirModal(thumb.src || alarma.photoUrl);
      });
    }

    item.appendChild(thumb);
    item.appendChild(info);
    item.appendChild(badge);

    historyList.appendChild(item);
  });
}

async function refrescarDashboardSuave() {
  if (!alarmaId || !miUid) return;

  try {
    const [estadoSnap, usuariosSnap, historialSnap] = await Promise.all([
      db.ref('alarmas/' + alarmaId + '/estadoDispositivo').once('value'),
      db.ref('alarmas/' + alarmaId + '/usuarios').once('value'),
      db.ref('alarmas/' + alarmaId + '/historial').once('value')
    ]);

    let estadoFinal = estadoSnap.val() || null;
    let historialFinal = historialSnap.val() || {};

    if (WEB_COMPAT_ALARMA_ID && WEB_COMPAT_ALARMA_ID !== alarmaId) {
      const [estadoCompatSnap, historialCompatSnap] = await Promise.all([
        db.ref('alarmas/' + WEB_COMPAT_ALARMA_ID + '/estadoDispositivo').once('value'),
        db.ref('alarmas/' + WEB_COMPAT_ALARMA_ID + '/historial').once('value')
      ]);
      estadoFinal = elegirEstadoMasReciente(estadoFinal, estadoCompatSnap.val() || null);
      historialFinal = mergeHistorial(historialFinal, historialCompatSnap.val() || {});
    }

    ultimoEstadoDispositivoCache = estadoFinal;
    actualizarEstadoDispositivoUI(ultimoEstadoDispositivoCache);
    renderUsuariosLista(usuariosSnap.val() || {});
    renderHistorialLista(historialFinal);
  } catch (e) {
    console.warn('No se pudo hacer refresco suave del dashboard:', e.message || e);
  }
}

function obtenerPathStorageDesdeEvento(alarma) {
  const path = (alarma && typeof alarma.storagePath === 'string') ? alarma.storagePath.trim() : '';
  if (path) return path;

  const url = (alarma && typeof alarma.photoUrl === 'string') ? alarma.photoUrl : '';
  if (!url) return '';

  try {
    const marker = '/o/';
    const idx = url.indexOf(marker);
    if (idx === -1) return '';
    const encodedPart = url.substring(idx + marker.length).split('?')[0];
    return decodeURIComponent(encodedPart || '').trim();
  } catch (_) {
    return '';
  }
}

function crearMiniaturaEvento(alarma) {
  const img = document.createElement('img');
  img.className = 'history-thumb';
  img.alt = 'Foto de movimiento';

  const srcInicial = (alarma && typeof alarma.photoUrl === 'string') ? alarma.photoUrl : '';
  const storagePath = obtenerPathStorageDesdeEvento(alarma);

  if (storagePath) {
    firebase.storage().ref().child(storagePath).getDownloadURL()
      .then((url) => {
        if (url) img.src = url;
      })
      .catch(() => {
        if (srcInicial) img.src = srcInicial;
      });
  } else if (srcInicial) {
    img.src = srcInicial;
  }

  return img;
}

async function eliminarFotoStoragePorEvento(alarma) {
  const storagePath = obtenerPathStorageDesdeEvento(alarma);
  if (!storagePath) return;

  try {
    await firebase.storage().ref().child(storagePath).delete();
  } catch (e) {
    const code = e && e.code ? e.code : '';
    // Ignora si ya no existe en Storage.
    if (code !== 'storage/object-not-found') {
      console.warn('No se pudo borrar foto de Storage:', storagePath, code || e.message || e);
    }
  }
}

async function limpiarHistorialAntiguoEnSegundoPlano(historialData, forzar = false) {
  if (!alarmaId || !miUid || miUid !== adminUid) return;
  if (limpiezaRetencionEnCurso) return;

  const ahora = Date.now();
  if (!forzar && (ahora - ultimaLimpiezaRetencionMs) < LIMPIEZA_RETENCION_INTERVALO_MS) {
    return;
  }

  const data = historialData || {};
  const limite = ahora - RETENCION_MS_HISTORIAL;
  const candidatos = Object.entries(data).filter(([_, evento]) => {
    if (!evento || typeof evento !== 'object') return false;
    const ts = Number(evento.timestamp || 0);
    return ts > 0 && ts < limite;
  });

  if (candidatos.length === 0) {
    ultimaLimpiezaRetencionMs = ahora;
    return;
  }

  limpiezaRetencionEnCurso = true;
  try {
    for (const [eventoId, evento] of candidatos) {
      await eliminarFotoStoragePorEvento(evento);
      await db.ref('alarmas/' + alarmaId + '/historial/' + eventoId).remove();
    }
    ultimaLimpiezaRetencionMs = Date.now();
  } catch (e) {
    console.warn('No se pudo completar limpieza de retencion semanal:', e.message || e);
  } finally {
    limpiezaRetencionEnCurso = false;
  }
}

async function liberarCupoAlCerrarSesion() {
  if (!alarmaId || !miUid) return;

  try {
    const usuariosRef = db.ref('alarmas/' + alarmaId + '/usuarios');
    const usuariosSnap = await usuariosRef.once('value');
    const usuarios = usuariosSnap.val() || {};

    const eraAdmin = miUid === adminUid;
    if (eraAdmin) {
      const siguienteAdmin = Object.keys(usuarios).find((uid) => uid !== miUid);
      const adminRef = db.ref('alarmas/' + alarmaId + '/adminUid');
      if (siguienteAdmin) {
        await adminRef.set(siguienteAdmin);
      } else {
        await adminRef.remove();
      }
    }

    await db.ref('alarmas/' + alarmaId + '/usuarios/' + miUid).remove();
    await db.ref('userAlarma/' + miUid).remove();
  } catch (e) {
    console.warn('No se pudo liberar el cupo al cerrar sesion:', e.message || e);
  }
}

document.getElementById('btn-logout').addEventListener('click', async () => {
  // Cerrar sesion no debe borrar la vinculacion del usuario a la alarma.
  // La baja explicita de miembros se hace desde el boton "Eliminar" (admin).
  auth.signOut().then(() => {
    window.location.href = 'index.html';
  });
});

db.ref('.info/connected').on('value', (snapshot) => {
  const conectado = snapshot.val() === true;
  connectionStatus.className = `status-pill ${conectado ? 'status-ok' : 'status-error'}`;
  connectionStatus.textContent = conectado ? 'En linea' : 'Sin conexion';
});

db.ref('.info/serverTimeOffset').on('value', (snapshot) => {
  const offset = Number(snapshot.val() || 0);
  serverTimeOffsetMs = Number.isFinite(offset) ? offset : 0;
});

// ---------- Utilidades ----------
function formatearFecha(timestampMs) {
  if (!timestampMs) return 'Sin fecha';
  const fecha = new Date(timestampMs);
  return fecha.toLocaleString('es-AR', {
    day: '2-digit', month: '2-digit', year: 'numeric',
    hour: '2-digit', minute: '2-digit', second: '2-digit'
  });
}

function tiempoRelativo(timestampMs) {
  if (!timestampMs) return '';
  const segundos = Math.floor((Date.now() - timestampMs) / 1000);
  if (segundos < 60) return `hace ${segundos}s`;
  const minutos = Math.floor(segundos / 60);
  if (minutos < 60) return `hace ${minutos} min`;
  const horas = Math.floor(minutos / 60);
  if (horas < 24) return `hace ${horas} h`;
  const dias = Math.floor(horas / 24);
  return `hace ${dias} d`;
}

function abrirModal(url) {
  document.getElementById('modal-img').src = url;
  document.getElementById('modal-overlay').classList.add('open');
}

document.getElementById('modal-overlay').addEventListener('click', () => {
  document.getElementById('modal-overlay').classList.remove('open');
});

function setDotEstado(dot, activo) {
  if (!dot) return;
  dot.classList.toggle('alert', !activo);
}

function actualizarEstadoDispositivoUI(estado) {
  if (!statusCamera) return;

  if (!estado || typeof estado !== 'object') {
    statusCamera.textContent = 'Sin conexion';
    setDotEstado(dotCamera, false);
    return;
  }

  const lastSeen = Number(estado.lastSeen || 0);
  const onlineReciente = !!lastSeen && (ahoraServidorMs() - lastSeen) < VENTANA_ONLINE_CAMARA_MS;
  const camOk = onlineReciente;

  setDotEstado(dotCamera, camOk);
  statusCamera.textContent = camOk ? 'Conectada' : 'Sin conexion';
}

// Los PIR no tienen forma de "avisar" que están conectados: solo informan
// cuando detectan movimiento. Por eso su estado se calcula a partir del
// historial de alarmas (última vez que dispararon), no de una conexión.
function actualizarEstadoSensorPIR(dot, statusEl, ultimoTimestamp) {
  if (!statusEl) return;

  if (!ultimoTimestamp) {
    statusEl.textContent = 'Sin datos aún';
    setDotEstado(dot, false);
    return;
  }

  const segundosDesde = (Date.now() - ultimoTimestamp) / 1000;
  const esReciente = segundosDesde < 30;

  setDotEstado(dot, esReciente);
  statusEl.textContent = esReciente
    ? 'Movimiento detectado ahora'
    : `Última vez: ${tiempoRelativo(ultimoTimestamp)}`;
}

function actualizarSyncAlert() {
  if (!tieneErrorUsuarios && !tieneErrorHistorial) {
    syncAlert.style.display = 'none';
    syncAlert.textContent = '';
    return;
  }

  const mensajes = [];
  if (tieneErrorUsuarios) mensajes.push('usuarios');
  if (tieneErrorHistorial) mensajes.push('historial');

  syncAlert.style.display = 'block';
  syncAlert.textContent = `No se pudo sincronizar: ${mensajes.join(' y ')}. Se reintentara automaticamente.`;
}

// ---------- Escuchas en tiempo real (una vez que sabemos la alarmaId) ----------
function iniciarEscuchas() {
  escucharEstadoDispositivo();
  escucharUsuarios();
  escucharHistorial();
  escucharEstadoComandoManual();
}

function reevaluarEstadoComandoManual() {
  if (!ultimoComandoManual) return;
  aplicarEstadoComandoManual(ultimoComandoManual);
}

function aplicarEstadoComandoManual(cmd) {
  if (!cmd || typeof cmd !== 'object') {
    ultimoComandoManual = null;
    ultimoEstadoComandoManualTs = 0;
    if (btnPhotoNow) btnPhotoNow.disabled = false;
    setManualPhotoStatus('Listo para enviar solicitud.', 'normal');
    return;
  }

  ultimoComandoManual = cmd;

  const ahoraMs = ahoraServidorMs();
  const tsRaw = Number(cmd.actualizadoEn || cmd.solicitadoEn || 0);
  // Protege contra timestamps corruptos/futuros que bloquean la UI.
  const ts = tsRaw > (ahoraMs + 60000) ? ahoraMs : tsRaw;
  if (ultimoEstadoComandoManualTs > (ahoraMs + 60000)) {
    ultimoEstadoComandoManualTs = 0;
  }
  if (ts && ts + 2000 < ultimoEstadoComandoManualTs) return;
  if (ts) ultimoEstadoComandoManualTs = ts;

  const estado = (cmd.estado || '').toLowerCase();
  const detalle = cmd.detalle || '';
  const edadMs = ts ? (ahoraMs - ts) : 0;

  if (estado === 'pendiente') {
    if (edadMs > MAX_MS_PENDIENTE_MANUAL) {
      setManualPhotoStatus('Solicitud anterior vencida. Podés intentar de nuevo.', 'error');
      if (btnPhotoNow) btnPhotoNow.disabled = false;
      return;
    }
    setManualPhotoStatus('Solicitud enviada. Esperando que el dispositivo tome la foto.', 'normal');
    if (btnPhotoNow) btnPhotoNow.disabled = true;
    return;
  }

  if (estado === 'procesando') {
    if (edadMs > MAX_MS_PROCESANDO_MANUAL) {
      const autoReintentado = !!cmd.autoReintentado;
      if (!autoReintentado && alarmaId) {
        const reintento = {
          tipo: 'captura_manual',
          solicitadoPor: miUid || (cmd.solicitadoPor || ''),
          solicitadoEn: firebase.database.ServerValue.TIMESTAMP,
          estado: 'pendiente',
          autoReintentado: true,
          detalle: 'Reintento automatico por timeout de procesamiento'
        };
        const updates = {};
        updates['alarmas/' + alarmaId + '/comandos/capturaManual'] = reintento;
        if (WEB_COMPAT_ALARMA_ID && WEB_COMPAT_ALARMA_ID !== alarmaId) {
          updates['alarmas/' + WEB_COMPAT_ALARMA_ID + '/comandos/capturaManual'] = reintento;
        }
        db.ref().update(updates).catch(() => {});
        setManualPhotoStatus('Reintentando captura automaticamente...', 'normal');
        if (btnPhotoNow) btnPhotoNow.disabled = true;
        return;
      }

      setManualPhotoStatus('La captura tardó demasiado. Podés reintentar.', 'error');
      if (btnPhotoNow) btnPhotoNow.disabled = false;
      return;
    }
    setManualPhotoStatus(detalle || 'Procesando captura manual...', 'normal');
    if (btnPhotoNow) btnPhotoNow.disabled = true;
    return;
  }

  if (estado === 'completado') {
    setManualPhotoStatus(detalle || 'Captura manual completada.', 'ok');
    if (btnPhotoNow) btnPhotoNow.disabled = false;
    return;
  }

  if (estado === 'error') {
    setManualPhotoStatus(detalle || 'Error en captura manual.', 'error');
    if (btnPhotoNow) btnPhotoNow.disabled = false;
  }
}

function escucharEstadoComandoManual() {
  if (!alarmaId) return;

  const rutaSesion = 'alarmas/' + alarmaId + '/comandos/capturaManual';
  const rutaCompat = WEB_COMPAT_ALARMA_ID ? ('alarmas/' + WEB_COMPAT_ALARMA_ID + '/comandos/capturaManual') : rutaSesion;

  db.ref(rutaSesion).on('value', (snapshot) => {
    aplicarEstadoComandoManual(snapshot.val());
  });

  if (rutaCompat !== rutaSesion) {
    db.ref(rutaCompat).on('value', (snapshot) => {
      aplicarEstadoComandoManual(snapshot.val());
    });
  }
}

async function solicitarFotoManual() {
  if (!alarmaId || !miUid) {
    setManualPhotoStatus('No se pudo enviar: sesión no lista.', 'error');
    return;
  }

  if (btnPhotoNow) btnPhotoNow.disabled = true;
  setManualPhotoStatus('Enviando solicitud de foto...', 'normal');

  try {
    const comando = {
      tipo: 'captura_manual',
      solicitadoPor: miUid,
      solicitadoEn: firebase.database.ServerValue.TIMESTAMP,
      estado: 'pendiente'
    };

    const updates = {};
    updates['alarmas/' + alarmaId + '/comandos/capturaManual'] = comando;

    // Compatibilidad con firmware: refleja el comando en el canal fijo
    // cuando el ID de sesión no coincide con el canal del dispositivo.
    if (WEB_COMPAT_ALARMA_ID && WEB_COMPAT_ALARMA_ID !== alarmaId) {
      updates['alarmas/' + WEB_COMPAT_ALARMA_ID + '/comandos/capturaManual'] = comando;
    }

    await db.ref().update(updates);

    setManualPhotoStatus('Solicitud enviada. Esperando que el dispositivo tome la foto.', 'ok');
  } catch (e) {
    setManualPhotoStatus('Error al enviar solicitud: ' + (e.message || e), 'error');
  } finally {
    if (btnPhotoNow) btnPhotoNow.disabled = false;
  }
}

if (btnPhotoNow) {
  btnPhotoNow.addEventListener('click', solicitarFotoManual);
}

function escucharEstadoDispositivo() {
  db.ref('alarmas/' + alarmaId + '/estadoDispositivo').on('value', (snapshot) => {
    ultimoEstadoDispositivoCache = elegirEstadoMasReciente(ultimoEstadoDispositivoCache, snapshot.val() || null);
    actualizarEstadoDispositivoUI(ultimoEstadoDispositivoCache);
  }, () => {
    actualizarEstadoDispositivoUI(ultimoEstadoDispositivoCache);
  });

  if (WEB_COMPAT_ALARMA_ID && WEB_COMPAT_ALARMA_ID !== alarmaId) {
    db.ref('alarmas/' + WEB_COMPAT_ALARMA_ID + '/estadoDispositivo').on('value', (snapshot) => {
      ultimoEstadoDispositivoCache = elegirEstadoMasReciente(ultimoEstadoDispositivoCache, snapshot.val() || null);
      actualizarEstadoDispositivoUI(ultimoEstadoDispositivoCache);
    }, () => {
      actualizarEstadoDispositivoUI(ultimoEstadoDispositivoCache);
    });
  }
}

function escucharUsuarios() {
  db.ref('alarmas/' + alarmaId + '/usuarios').on('value', (snapshot) => {
    tieneErrorUsuarios = false;
    actualizarSyncAlert();
    renderUsuariosLista(snapshot.val() || {});
    // Limpieza defensiva: elimina entradas en DB cuyo email ya no existe en Auth.
    limpiarUsuariosHuerfanosEnSegundoPlano(snapshot.val() || {});
  }, () => {
    tieneErrorUsuarios = true;
    actualizarSyncAlert();
  });
}

async function verificarSiSePuedeConsultarAuthPorEmail() {
  if (puedeVerificarAuthPorEmail !== null) return puedeVerificarAuthPorEmail;
  const user = auth.currentUser;
  if (!user || !user.email) {
    puedeVerificarAuthPorEmail = false;
    return false;
  }

  try {
    const metodos = await auth.fetchSignInMethodsForEmail(user.email);
    // Si devuelve vacio para el usuario actual, esta proteccion impide validar por email.
    puedeVerificarAuthPorEmail = Array.isArray(metodos) && metodos.length > 0;
  } catch (e) {
    console.warn('No se pudo validar disponibilidad de lookup en Auth:', e.message || e);
    puedeVerificarAuthPorEmail = false;
  }

  return puedeVerificarAuthPorEmail;
}

async function limpiarUsuariosHuerfanosEnSegundoPlano(usuariosData) {
  if (!alarmaId || !miUid || miUid !== adminUid) return;

  const ahora = Date.now();
  if ((ahora - ultimaLimpiezaHuerfanosMs) < 60000) return;
  ultimaLimpiezaHuerfanosMs = ahora;

  const puedeConsultar = await verificarSiSePuedeConsultarAuthPorEmail();
  if (!puedeConsultar) return;

  const entradas = Object.entries(usuariosData || {});
  for (const [uid, usuario] of entradas) {
    if (!usuario || typeof usuario !== 'object') continue;
    if (uid === miUid) continue;

    const email = (usuario.email || '').trim().toLowerCase();
    if (!email) continue;

    try {
      const metodos = await auth.fetchSignInMethodsForEmail(email);
      const existeEnAuth = Array.isArray(metodos) && metodos.length > 0;
      if (!existeEnAuth) {
        await db.ref('alarmas/' + alarmaId + '/usuarios/' + uid).remove();
        await db.ref('userAlarma/' + uid).remove();
      }
    } catch (e) {
      console.warn('No se pudo validar/eliminar usuario huerfano:', uid, e.message || e);
    }
  }
}

function escucharHistorial() {
  const alarmasRef = db.ref('alarmas/' + alarmaId + '/historial')
    .orderByChild('timestamp')
    .limitToLast(100);

  alarmasRef.on('value', (snapshot) => {
    tieneErrorHistorial = false;
    actualizarSyncAlert();
    limpiarHistorialAntiguoEnSegundoPlano(snapshot.val() || {});
    renderHistorialLista(snapshot.val() || {});
  }, () => {
    tieneErrorHistorial = true;
    actualizarSyncAlert();
  });

  if (WEB_COMPAT_ALARMA_ID && WEB_COMPAT_ALARMA_ID !== alarmaId) {
    const alarmasCompatRef = db.ref('alarmas/' + WEB_COMPAT_ALARMA_ID + '/historial')
      .orderByChild('timestamp')
      .limitToLast(100);

    alarmasCompatRef.on('value', async (snapshot) => {
      tieneErrorHistorial = false;
      actualizarSyncAlert();
      try {
        const principalSnap = await db.ref('alarmas/' + alarmaId + '/historial').once('value');
        const merged = mergeHistorial(principalSnap.val() || {}, snapshot.val() || {});
        renderHistorialLista(merged);
      } catch (e) {
        renderHistorialLista(snapshot.val() || {});
      }
    }, () => {
      // Si falla el canal compat, mantenemos el canal principal.
      actualizarSyncAlert();
    });
  }
}
