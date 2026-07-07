const form = document.getElementById('form-login');
const btnLogin = document.getElementById('btn-login');
const btnRegistro = document.getElementById('btn-registro');
const errorMsg = document.getElementById('error-msg');

function mostrarError(mensaje) {
  errorMsg.textContent = mensaje;
  errorMsg.style.display = 'block';
}

function ocultarError() {
  errorMsg.style.display = 'none';
}

async function asegurarPersistenciaLocal() {
  try {
    await auth.setPersistence(firebase.auth.Auth.Persistence.LOCAL);
  } catch (e) {
    console.error('No se pudo fijar persistencia LOCAL:', e);
  }
}

function traducirError(codigo) {
  const mensajes = {
    'auth/invalid-email': 'El correo electrónico no es válido.',
    'auth/user-not-found': 'No existe una cuenta con ese correo.',
    'auth/wrong-password': 'Contraseña incorrecta.',
    'auth/invalid-credential': 'Correo o contraseña incorrectos.',
    'auth/email-already-in-use': 'Ya existe una cuenta con ese correo.',
    'auth/weak-password': 'La contraseña debe tener al menos 6 caracteres.',
  };
  return mensajes[codigo] || 'Ocurrió un error. Intentá de nuevo.';
}

// Si ya hay sesión iniciada, ir directo al dashboard
auth.onAuthStateChanged((user) => {
  if (user) {
    window.location.href = 'dashboard.html';
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

form.addEventListener('submit', (e) => {
  e.preventDefault();
  ocultarError();

  const email = document.getElementById('email').value.trim();
  const password = document.getElementById('password').value;

  btnLogin.disabled = true;
  btnLogin.classList.add('loading');
  btnLogin.textContent = 'Ingresando...';

  asegurarPersistenciaLocal().then(() => auth.signInWithEmailAndPassword(email, password))
    .then(() => {
      window.location.href = 'dashboard.html';
    })
    .catch((err) => {
      mostrarError(traducirError(err.code));
      document.getElementById('email').value = '';
      document.getElementById('password').value = '';
      btnLogin.disabled = false;
      btnLogin.classList.remove('loading');
      btnLogin.textContent = 'Ingresar';
    });
});

if (btnRegistro) {
  btnRegistro.addEventListener('click', () => {
    ocultarError();

    const email = document.getElementById('email').value.trim();
    const password = document.getElementById('password').value;

    if (!email || !password) {
      mostrarError('Completá correo y contraseña para crear la cuenta.');
      return;
    }

    asegurarPersistenciaLocal().then(() => auth.createUserWithEmailAndPassword(email, password))
      .then(() => {
        window.location.href = 'dashboard.html';
      })
      .catch((err) => {
        mostrarError(traducirError(err.code));
        document.getElementById('email').value = '';
        document.getElementById('password').value = '';
      });
  });
}
