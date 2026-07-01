const form = document.getElementById('form-activar');
const btnActivar = document.getElementById('btn-activar');
const errorMsg = document.getElementById('error-msg');

const MAX_USUARIOS = 4;

function mostrarError(mensaje) {
  errorMsg.textContent = mensaje;
  errorMsg.style.display = 'block';
}

function ocultarError() {
  errorMsg.style.display = 'none';
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
  const nombre = document.getElementById('nombre').value.trim();
  const email = document.getElementById('email').value.trim();
  const password = document.getElementById('password').value;

  btnActivar.disabled = true;
  btnActivar.textContent = 'Verificando...';

  let userCredential;

  try {
    // 1) Crear la cuenta (esto deja al usuario autenticado, necesario
    //    para poder leer /codigosActivacion con las reglas de seguridad)
    userCredential = await auth.createUserWithEmailAndPassword(email, password);
    const uid = userCredential.user.uid;

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
      email: email,
      fechaAlta: firebase.database.ServerValue.TIMESTAMP
    });

    // 5) Guardar el mapeo para saber a qué alarma pertenece este usuario
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

    btnActivar.disabled = false;
    btnActivar.textContent = 'Activar y crear cuenta';
  }
});
