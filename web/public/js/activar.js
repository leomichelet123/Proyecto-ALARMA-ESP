const form = document.getElementById('form-activar');
const btnActivar = document.getElementById('btn-activar');
const errorMsg = document.getElementById('error-msg');
const camposCuenta = document.getElementById('campos-cuenta');

const MAX_USUARIOS = 4;
let usuarioYaLogueado = null;

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
    'auth/weak-password': 'La contraseña debe tener al menos 6 caracteres.',
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

  if (codigo !== 'ABC123XY') {
    mostrarError('Código de activación inválido.');
    btnActivar.disabled = false;
    btnActivar.textContent = 'Activar y crear cuenta';
    return;
  }

  btnActivar.disabled = true;
  btnActivar.classList.add('loading');
  btnActivar.textContent = 'Verificando...';

  let userCredential;

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
      userCredential = await auth.createUserWithEmailAndPassword(email, password);
      uid = userCredential.user.uid;
      emailFinal = email;
    }

    // 2) Validar el código de activación
    const codigoSnap = await db.ref('codigosActivacion/' + codigo).once('value');
    const codigoData = codigoSnap.val();

    if (!codigoData || codigoData.activo !== true) {
      throw new Error('CODIGO_INVALIDO');
    }

    const alarmaId = codigoData.alarmaId;

    // 3) Chequear que no se haya llegado al máximo de usuarios
    const usuariosSnap = await db.ref('alarmas/' + alarmaId + '/usuarios').once('value');
    const cantidadActual = usuariosSnap.numChildren();

    if (cantidadActual >= MAX_USUARIOS) {
      throw new Error('LIMITE_ALCANZADO');
    }

    // 4) Registrar al usuario dentro de la alarma
    await db.ref('alarmas/' + alarmaId + '/usuarios/' + uid).set({
      nombre: nombre,
      apellido: apellido,
      email: emailFinal,
      fechaAlta: firebase.database.ServerValue.TIMESTAMP
    });

    // 5) Si es el primer usuario, asignarlo como admin
    const adminSnap = await db.ref('alarmas/' + alarmaId + '/adminUid').once('value');
    if (!adminSnap.val()) {
      await db.ref('alarmas/' + alarmaId + '/adminUid').set(uid);
    }

    // 6) Guardar el mapeo para saber a qué alarma pertenece este usuario
    await db.ref('userAlarma/' + uid).set(alarmaId);

    window.location.href = 'dashboard.html';

  } catch (err) {
    // Si algo falló después de crear la cuenta, la borramos para no
    // dejar cuentas huérfanas sin acceso a ninguna alarma
    if (userCredential && userCredential.user) {
      await userCredential.user.delete().catch(() => {});
    }

    if (err.message === 'CODIGO_INVALIDO') {
      mostrarError('El código de activación no es válido.');
    } else if (err.message === 'LIMITE_ALCANZADO') {
      mostrarError('Esta alarma ya tiene el máximo de 4 usuarios.');
    } else if (err.code) {
      mostrarError(traducirError(err.code));
    } else {
      mostrarError('Ocurrió un error. Intentá de nuevo.');
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
