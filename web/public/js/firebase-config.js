// Configuración de tu proyecto Firebase
const firebaseConfig = {
  apiKey: "AIzaSyCwpDqIEPkD89iA5MsEpnqNBXemJ1aqeTo",
  authDomain: "alarma-esp32-abc0c.firebaseapp.com",
  databaseURL: "https://alarma-esp32-abc0c-default-rtdb.firebaseio.com",
  projectId: "alarma-esp32-abc0c",
  storageBucket: "alarma-esp32-abc0c.firebasestorage.app",
  messagingSenderId: "341792347405",
  appId: "1:341792347405:web:479b8434c935e050e6cc44"
};

firebase.initializeApp(firebaseConfig);
const auth = firebase.auth();
const db = firebase.database();
