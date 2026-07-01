let alarmaId = null;
let miUid = null;

// ---------- Protección de ruta + resolver a qué alarma pertenezco ----------
auth.onAuthStateChanged(async (user) => {
  if (!user) {
    window.location.href = 'index.html';
    return;
  }

  miUid = user.uid;
  document.getElementById('user-email').textContent = user.email;

  const snap = await db.ref('userAlarma/' + user.uid).once('value');
  alarmaId = snap.val();

  if (!alarmaId) {
    // Cuenta sin alarma asignada (no debería pasar en uso normal)
    await auth.signOut();
    window.location.href = 'index.html';
    return;
  }

  iniciarEscuchas();
});

document.getElementById('btn-logout').addEventListener('click', () => {
  auth.signOut().then(() => {
    window.location.href = 'index.html';
  });
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
    status.textContent = 'Sin datos aún';
    return;
  }

  const segundosDesde = (Date.now() - ultimoTimestamp) / 1000;
  const esReciente = segundosDesde < 30;

  dot.classList.toggle('alert', esReciente);
  status.textContent = `Última alarma: ${tiempoRelativo(ultimoTimestamp)}`;
}

// ---------- Escuchas en tiempo real (una vez que sabemos la alarmaId) ----------
function iniciarEscuchas() {
  escucharUsuarios();
  escucharHistorial();
}

function escucharUsuarios() {
  const usersList = document.getElementById('users-list');

  db.ref('alarmas/' + alarmaId + '/usuarios').on('value', (snapshot) => {
    const data = snapshot.val() || {};
    usersList.innerHTML = '';

    Object.entries(data).forEach(([uid, u]) => {
      const item = document.createElement('div');
      item.className = 'history-item';
      item.style.gridTemplateColumns = '1fr auto';

      const esUnoMismo = uid === miUid;

      item.innerHTML = `
        <div>
          <div class="history-sensor">${u.nombre}${esUnoMismo ? ' (vos)' : ''}</div>
          <div class="history-time">${u.email}</div>
        </div>
      `;

      if (!esUnoMismo) {
        const btnEliminar = document.createElement('button');
        btnEliminar.textContent = 'Eliminar';
        btnEliminar.className = 'link-btn';
        btnEliminar.style.color = 'var(--alert)';
        btnEliminar.addEventListener('click', () => {
          if (confirm(`¿Quitar a ${u.nombre} de esta alarma?`)) {
            db.ref('alarmas/' + alarmaId + '/usuarios/' + uid).remove();
          }
        });
        item.appendChild(btnEliminar);
      }

      usersList.appendChild(item);
    });
  });
}

function escucharHistorial() {
  const historyList = document.getElementById('history-list');
  const emptyState = document.getElementById('empty-state');

  const alarmasRef = db.ref('alarmas/' + alarmaId + '/historial')
    .orderByChild('timestamp')
    .limitToLast(100);

  alarmasRef.on('value', (snapshot) => {
    const data = snapshot.val();
    historyList.innerHTML = '';

    if (!data) {
      emptyState.style.display = 'block';
      actualizarEstadoSensor(1, null);
      actualizarEstadoSensor(2, null);
      return;
    }

    emptyState.style.display = 'none';

    const alarmas = Object.values(data).sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));

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
  });
}
