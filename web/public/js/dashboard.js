let alarmaId = null;
let miUid = null;
let tieneErrorUsuarios = false;
let tieneErrorHistorial = false;
let ultimoTimestampVisto = null;

const connectionStatus = document.getElementById('connection-status');
const syncAlert = document.getElementById('sync-alert');
const alarmBanner = document.getElementById('alarm-banner');

// ---------- Notificaciones ----------
function cerrarBanner() {
  alarmBanner.classList.remove('visible');
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

      // Si no existe su nodo por UID actual, lo crea con datos mínimos válidos.
      if (!yaEstaPorUid) {
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
        document.getElementById('status-sensor-1').textContent = 'Cuenta sin alarma vinculada';
        document.getElementById('status-sensor-2').textContent = 'Cuenta sin alarma vinculada';
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
  // Notificaciones fuera del bloque crítico para que no afecten el dashboard
  iniciarNotificacionesPush();
});

document.getElementById('btn-logout').addEventListener('click', () => {
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

function actualizarEstadoSensor(sensorId, ultimoTimestamp) {
  const dot = document.getElementById(`dot-sensor-${sensorId}`);
  const status = document.getElementById(`status-sensor-${sensorId}`);

  if (!ultimoTimestamp) {
    status.textContent = 'Pendiente de configurar ESP32-CAM';
    dot.classList.remove('alert');
    return;
  }

  const segundosDesde = (Date.now() - ultimoTimestamp) / 1000;
  const esReciente = segundosDesde < 30;

  dot.classList.toggle('alert', esReciente);
  status.textContent = `Última alarma: ${tiempoRelativo(ultimoTimestamp)}`;
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
  escucharUsuarios();
  escucharHistorial();
}

function escucharUsuarios() {
  const usersList = document.getElementById('users-list');

  db.ref('alarmas/' + alarmaId + '/usuarios').on('value', (snapshot) => {
    tieneErrorUsuarios = false;
    actualizarSyncAlert();

    const data = snapshot.val() || {};
    usersList.innerHTML = '';

    const total = Object.keys(data).length;
    const tituloUsuarios = document.querySelector('.section-title');
    // Actualiza el título con el contador
    const allTitles = document.querySelectorAll('.section-title');
    allTitles.forEach(t => {
      if (t.textContent.startsWith('Usuarios')) {
        t.textContent = `Usuarios de esta alarma (${total}/4)`;
      }
    });

    Object.entries(data).forEach(([uid, u]) => {
      const usuario = (u && typeof u === 'object') ? u : {};
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

      if (!esUnoMismo) {
        const btnEliminar = document.createElement('button');
        btnEliminar.textContent = 'Eliminar';
        btnEliminar.className = 'link-btn';
        btnEliminar.style.color = 'var(--alert)';
        btnEliminar.addEventListener('click', () => {
          if (confirm(`¿Quitar a ${nombre} de esta alarma?`)) {
            db.ref('alarmas/' + alarmaId + '/usuarios/' + uid).remove();
          }
        });
        item.appendChild(btnEliminar);
      }

      usersList.appendChild(item);
    });
  }, () => {
    tieneErrorUsuarios = true;
    actualizarSyncAlert();
  });
}

function escucharHistorial() {
  const historyList = document.getElementById('history-list');
  const emptyState = document.getElementById('empty-state');

  const alarmasRef = db.ref('alarmas/' + alarmaId + '/historial')
    .orderByChild('timestamp')
    .limitToLast(100);

  alarmasRef.on('value', (snapshot) => {
    tieneErrorHistorial = false;
    actualizarSyncAlert();

    const data = snapshot.val();
    historyList.innerHTML = '';

    if (!data) {
      emptyState.style.display = 'block';
      actualizarEstadoSensor(1, null);
      actualizarEstadoSensor(2, null);
      return;
    }

    emptyState.style.display = 'none';

    const alarmas = Object.values(data)
      .filter((a) => a && typeof a === 'object' && a.sensor !== undefined)
      .sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));

    if (alarmas.length === 0) {
      emptyState.style.display = 'block';
      actualizarEstadoSensor(1, null);
      actualizarEstadoSensor(2, null);
      return;
    }

    // Detectar si hay un evento nuevo (más reciente que el último visto)
    const masReciente = alarmas[0].timestamp || 0;
    if (ultimoTimestampVisto !== null && masReciente > ultimoTimestampVisto) {
      mostrarBanner();
      // Notificación nativa del navegador (si la página está en segundo plano)
      if (Notification.permission === 'granted') {
        new Notification('🚨 ALARMA ACTIVADA', {
          body: `Movimiento detectado — Sensor ${alarmas[0].sensor}`,
          icon: '/icon-192.png',
          requireInteraction: true
        });
      }
    }
    ultimoTimestampVisto = masReciente;

    const ultimaPorSensor = {};
    alarmas.forEach((alarma) => {
      if (!ultimaPorSensor[alarma.sensor]) {
        ultimaPorSensor[alarma.sensor] = alarma.timestamp;
      }
    });
    actualizarEstadoSensor(1, ultimaPorSensor[1]);
    actualizarEstadoSensor(2, ultimaPorSensor[2]);

    alarmas.forEach((alarma) => {
      const item = document.createElement('div');
      item.className = 'history-item';

      const foto = alarma.photoUrl
        ? `<img class="history-thumb" src="${alarma.photoUrl}" alt="Foto sensor ${alarma.sensor}">`
        : `<div class="history-thumb"></div>`;

      item.innerHTML = `
        ${foto}
        <div>
          <div class="history-sensor">Sensor ${alarma.sensor}</div>
          <div class="history-time">${formatearFecha(alarma.timestamp)}</div>
        </div>
        <span class="badge">Movimiento</span>
      `;

      if (alarma.photoUrl) {
        item.querySelector('.history-thumb').addEventListener('click', () => {
          abrirModal(alarma.photoUrl);
        });
      }

      historyList.appendChild(item);
    });
  }, () => {
    tieneErrorHistorial = true;
    actualizarSyncAlert();
  });
}
