importScripts('https://www.gstatic.com/firebasejs/10.13.0/firebase-app-compat.js');
importScripts('https://www.gstatic.com/firebasejs/10.13.0/firebase-messaging-compat.js');

// Misma config que firebase-config.js
firebase.initializeApp({
  apiKey: "AIzaSyCwpDqIEPkD89iA5MsEpnqNBXemJ1aqeTo",
  authDomain: "alarma-esp32-abc0c.firebaseapp.com",
  databaseURL: "https://alarma-esp32-abc0c-default-rtdb.firebaseio.com",
  projectId: "alarma-esp32-abc0c",
  storageBucket: "alarma-esp32-abc0c.firebasestorage.app",
  messagingSenderId: "341792347405",
  appId: "1:341792347405:web:479b8434c935e050e6cc44"
});

const messaging = firebase.messaging();

// Notificación cuando la app está en segundo plano o cerrada
messaging.onBackgroundMessage((payload) => {
  const { title, body } = payload.notification || {};
  self.registration.showNotification(title || '🚨 ALARMA ACTIVADA', {
    body: body || 'Se detectó movimiento en tu sistema AlarmaMB.',
    icon: '/icon-192.png',
    badge: '/icon-192.png',
    vibrate: [300, 100, 300, 100, 300],
    requireInteraction: true,
    data: { url: '/dashboard.html' }
  });
});

// Al hacer click en la notificación, abre el dashboard
self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  event.waitUntil(
    clients.matchAll({ type: 'window', includeUncontrolled: true }).then((clientList) => {
      for (const client of clientList) {
        if (client.url.includes('dashboard.html') && 'focus' in client) {
          return client.focus();
        }
      }
      return clients.openWindow('/dashboard.html');
    })
  );
});
