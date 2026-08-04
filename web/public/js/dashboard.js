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
const VENTANA_ONLINE_CAMARA_MS = 60000;
const REFRESCO_SUAVE_MS = 5000;
let ultimaLimpiezaRetencionMs = 0;
let limpiezaRetencionEnCurso = false;
let autoRefreshTimer = null;

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

  const usuariosPorEmail = new Map();
  Object.entries(data || {}).forEach(([uid, u]) => {
    const usuario = (u && typeof u === 'object') ? u : {};
    const emailNorm = (usuario.email || uid).toLowerCase();
    const actual = usuariosPorEmail.get(emailNorm);
    const fechaNueva = Number(usuario.fechaAlta || 0);
    const fechaActual = actual ? Number((actual.usuario && actual.usuario.fechaAlta) || 0) : -1;
    if (!actual || fechaNueva >= fechaActual) {
      usuariosPorEmail.set(emailNorm, { uid, usuario });
    }
  });

  const total = usuariosPorEmail.size;
  const allTitles = document.querySelectorAll('.section-title');
  allTitles.forEach(t => {
    if (t.textContent.startsWith('Usuarios')) {
      t.textContent = `Usuarios de esta alarma (${total}/4)`;
    }
  });

  Array.from(usuariosPorEmail.values()).forEach(({ uid, usuario }) => {
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
  if (ultimoTimestampVisto !== null && masReciente > ultimoTimestampVisto) {
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

  actualizarEstadoSensorPIR(dotPirGeneral, statusPirGeneral, masReciente);

  alarmas.forEach((alarma) => {
    const item = document.createElement('div');
    item.className = 'history-item';
    const esCapturaManual = (alarma.tipoEvento === 'captura_manual') || Number(alarma.sensor) === 0;
    const tituloEvento = esCapturaManual ? 'Captura manual' : 'Sensor de movimiento';
    const badgeEvento = esCapturaManual ? 'Captura' : 'Movimiento';
    const detalleRuta = esCapturaManual && alarma.storagePath
      ? `<div class="history-time">Guardado en: ${alarma.storagePath}</div>`
      : '';

    const thumb = alarma.photoUrl ? crearMiniaturaEvento(alarma) : document.createElement('div');
    if (!alarma.photoUrl) {
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

    if (alarma.photoUrl) {
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

    actualizarEstadoDispositivoUI(estadoSnap.val());
    renderUsuariosLista(usuariosSnap.val() || {});
    renderHistorialLista(historialSnap.val() || {});
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
  if (srcInicial) {
    img.src = srcInicial;
  }

  img.addEventListener('error', async () => {
    if (img.dataset.fallbackTried === '1') return;
    img.dataset.fallbackTried = '1';

    const storagePath = obtenerPathStorageDesdeEvento(alarma);
    if (!storagePath) return;

    try {
      const url = await firebase.storage().ref().child(storagePath).getDownloadURL();
      if (url && url !== img.src) {
        img.src = url;
      }
    } catch (e) {
      // Si tampoco resuelve por SDK, dejamos la miniatura en error.
    }
  });

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
  await liberarCupoAlCerrarSesion();
  auth.signOut().then(() => {
    window.location.href = 'index.html';
  });
});

db.ref('.info/connected').on('value', (snapshot) => {
  const conectado = snapshot.val() === true;
  connectionStatus.className = `status-pill ${conectado ? 'status-ok' : 'status-error'}`;
  connectionStatus.textContent = conectado ? 'En linea' : 'Sin conexion';
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
  const online = !!lastSeen && (Date.now() - lastSeen) < VENTANA_ONLINE_CAMARA_MS;
  const camOk = online && estado.camaraOk === true;

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
}

async function solicitarFotoManual() {
  if (!alarmaId || !miUid) {
    setManualPhotoStatus('No se pudo enviar: sesión no lista.', 'error');
    return;
  }

  if (btnPhotoNow) btnPhotoNow.disabled = true;
  setManualPhotoStatus('Enviando solicitud de foto...', 'normal');

  try {
    await db.ref('alarmas/' + alarmaId + '/comandos/capturaManual').set({
      tipo: 'captura_manual',
      solicitadoPor: miUid,
      solicitadoEn: firebase.database.ServerValue.TIMESTAMP,
      estado: 'pendiente'
    });

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
    actualizarEstadoDispositivoUI(snapshot.val());
  }, () => {
    actualizarEstadoDispositivoUI(null);
  });
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
}
