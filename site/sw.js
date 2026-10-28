// Offline copy for the home-screen app: network first (so deploys show up at once), cache as the fallback.
const CACHE = 'spiro-v3';
self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(['/', 'JetBrainsMono-Regular.woff2', 'favicon.svg', 'icon-192.png', 'theme.js'])));
  self.skipWaiting();
});
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== location.origin) return;
  const key = req.mode === 'navigate' && new URL(req.url).pathname === '/' ? '/' : req;       // only the home page is the offline start page
  e.respondWith(fetch(req).then((res) => {
    if (res.ok) { const copy = res.clone(); caches.open(CACHE).then((c) => c.put(key, copy)); }
    return res;
  }).catch(() => caches.match(key)));
});
