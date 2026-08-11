const form = document.getElementById('form-activar');
const btnActivar = document.getElementById('btn-activar');
const errorMsg = document.getElementById('error-msg');
const camposCuenta = document.getElementById('campos-cuenta');

const MAX_USUARIOS = 4;
let usuarioYaLogueado = null;
const CODIGO_ACTIVACION_FIJO = 'ABC123XY';
const ALARMA_ID_FIJA = 'alarma001';

// Si el usuario ya tiene sesión, ocultar campos de email/password
auth.onAuthStateChanged((user) => {
  if (user) {
    usuarioYaLogueado = user;
    camposCuenta.style.display = 'none';
    document.getElementById('email').removeAttribute('required');
    document.getElementById('password').removeAttribute('required');
    btnActivar.textContent = 'Vincular cuenta a esta alarma';
  } else {
    usuarioYaLogueado = null;
    camposCuenta.style.display = 'block';
    document.getElementById('email').setAttribute('required', '');
    document.getElementById('password').setAttribute('required', '');
    btnActivar.textContent = 'Activar y crear cuenta';
  }
});

async function asegurarPersistenciaLocal() {
  try {
    await auth.setPersistence(firebase.auth.Auth.Persistence.LOCAL);
  } catch (e) {
    console.error('No se pudo fijar persistencia LOCAL:', e);
  }
}

function mostrarError(mensaje) {
  errorMsg.textContent = mensaje;
  errorMsg.style.display = 'block';
}

function ocultarError() {
  errorMsg.style.display = 'none';
}

function sanitizarNombre(texto) {
  return texto.replace(/[^a-záéíóúñA-ZÁÉÍÓÚÑ\s]/g, '').trim();
}

function traducirError(codigo) {
  const mensajes = {
    'auth/invalid-email': 'El correo electrónico no es válido.',
    'auth/email-already-in-use': 'Ya existe una cuenta con ese correo.',
    'auth/wrong-password': 'La contraseña no coincide con ese correo.',
    'auth/user-not-found': 'No existe una cuenta con ese correo.',
    'auth/weak-password': 'La contraseña debe tener al menos 6 caracteres.',
    'auth/operation-not-allowed': 'Registro por correo/contraseña deshabilitado en Firebase Auth.',
    'auth/network-request-failed': 'Error de red. Revisá tu conexión e intentá de nuevo.',
    'PERMISSION_DENIED': 'Permiso denegado en Firebase. Revisá reglas/configuración.',
    'CODIGO_INVALIDO': 'El código de activación no es válido.',
    'LIMITE_ALCANZADO': 'Esta alarma ya tiene el máximo de 4 usuarios.',
    'NO_SE_PUDO_VINCULAR': 'No se pudo vincular la cuenta en este intento. Reintentá.'
  };
  return mensajes[codigo] || 'Ocurrió un error. Intentá de nuevo.';
}

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  ocultarError();

  const codigo = document.getElementById('codigo').value.trim().toUpperCase();
  const nombre = sanitizarNombre(document.getElementById('nombre').value);
  const apellido = sanitizarNombre(document.getElementById('apellido').value);
  const email = document.getElementById('email').value.trim();
  const password = document.getElementById('password').value;

  if (!nombre || !apellido) {
    mostrarError('Ingresá tu nombre y apellido (solo letras).');
    return;
  }

  if (codigo !== CODIGO_ACTIVACION_FIJO) {
    mostrarError('Código de activación inválido.');
    btnActivar.disabled = false;
    btnActivar.classList.remove('loading');
    btnActivar.textContent = 'Activar y crear cuenta';
    return;
  }

  btnActivar.disabled = true;
  btnActivar.classList.add('loading');
  btnActivar.textContent = 'Verificando...';

  let userCredential;
  let creoCuentaNueva = false;

  try {
    await asegurarPersistenciaLocal();

    let uid;
    let emailFinal;

    if (usuarioYaLogueado) {
      // Usuario ya logueado: usar su cuenta existente
      uid = usuarioYaLogueado.uid;
      emailFinal = usuarioYaLogueado.email;
    } else {
      // Usuario nuevo: crear cuenta
      const email = document.getElementById('email').value.trim();
      const password = document.getElementById('password').value;
      if (!email || !password) {
        mostrarError('Completá correo y contraseña.');
        btnActivar.disabled = false;
        btnActivar.textContent = 'Activar y crear cuenta';
        return;
      }
      try {
        userCredential = await auth.createUserWithEmailAndPassword(email, password);
        creoCuentaNueva = true;
      } catch (errCrear) {
        // Si el correo ya existe, usamos la cuenta existente en vez de fallar.
        if (errCrear.code === 'auth/email-already-in-use') {
          userCredential = await auth.signInWithEmailAndPassword(email, password);
        } else {
          throw errCrear;
        }
      }
      uid = userCredential.user.uid;
      emailFinal = email;
    }

    // 2) Alarma fija para flujo actual de la web.
    const alarmaId = ALARMA_ID_FIJA;

    // 3) Alta atomica por UID: evita sobreescrituras y respeta cupo maximo de 4.
    const usuariosRef = db.ref('alarmas/' + alarmaId + '/usuarios');
    let limiteAlcanzado = false;
    let uidYaRegistrado = false;

    const tx = await usuariosRef.transaction((actual) => {
      const usuarios = (actual && typeof actual === 'object') ? { ...actual } : {};

      if (usuarios[uid]) {
        uidYaRegistrado = true;
        return usuarios;
      }

      const cantidadActual = Object.keys(usuarios).length;
      if (cantidadActual >= MAX_USUARIOS) {
        limiteAlcanzado = true;
        return;
      }

      usuarios[uid] = {
        nombre: nombre,
        apellido: apellido,
        email: emailFinal,
        fechaAlta: firebase.database.ServerValue.TIMESTAMP
      };
      return usuarios;
    });

    if (!tx.committed) {
      if (limiteAlcanzado) {
        throw new Error('LIMITE_ALCANZADO');
      }
      throw new Error('NO_SE_PUDO_VINCULAR');
    }

    // 4) Guardar mapeo de cuenta -> alarma.
    // Si falla y era una alta nueva, revertimos para no dejar perfiles arrastrados.
    try {
      await db.ref('userAlarma/' + uid).set(alarmaId);
    } catch (e) {
      if (!uidYaRegistrado) {
        await db.ref('alarmas/' + alarmaId + '/usuarios/' + uid).remove().catch(() => {});
      }
      throw e;
    }

    // 5) Si es el primer usuario, intentar asignarlo como admin
    // (si falla por carrera, no bloquea el alta del usuario)
    const adminSnap = await db.ref('alarmas/' + alarmaId + '/adminUid').once('value');
    if (!adminSnap.val()) {
      try {
        await db.ref('alarmas/' + alarmaId + '/adminUid').set(uid);
      } catch (e) {
        console.warn('No se pudo asignar adminUid en este intento:', e.message || e);
      }
    }

    window.location.href = 'dashboard.html';

  } catch (err) {
    // Si algo falló después de crear la cuenta, la borramos para no
    // dejar cuentas huérfanas sin acceso a ninguna alarma
    if (creoCuentaNueva && userCredential && userCredential.user) {
      await userCredential.user.delete().catch(() => {});
    }

    if (err.message === 'CODIGO_INVALIDO' || err.message === 'LIMITE_ALCANZADO' || err.message === 'NO_SE_PUDO_VINCULAR') {
      mostrarError(traducirError(err.message));
    } else if (err.code) {
      mostrarError(traducirError(err.code));
    } else {
      const detalle = (err && (err.message || err.code)) ? ` (${err.message || err.code})` : '';
      mostrarError('Ocurrió un error. Intentá de nuevo.' + detalle);
    }

    document.getElementById('codigo').value = '';
    if (!usuarioYaLogueado) {
      document.getElementById('email').value = '';
      document.getElementById('password').value = '';
    }
    btnActivar.disabled = false;
    btnActivar.classList.remove('loading');
    btnActivar.textContent = 'Activar y crear cuenta';
  }
});

// Toggle show/hide password
document.querySelectorAll('.btn-toggle-password').forEach(btn => {
  btn.addEventListener('click', (e) => {
    e.preventDefault();
    const targetId = btn.getAttribute('data-target');
    const input = document.getElementById(targetId);
    if (input.type === 'password') {
      input.type = 'text';
      btn.textContent = '🙈';
    } else {
      input.type = 'password';
      btn.textContent = '👁️';
    }
  });
});
