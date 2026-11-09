// Service worker: keeps the disc data the game reads in this browser.
//
// The game reads disc files with synchronous ranged GETs from its worker
// threads (/disc/<file>, "Range: bytes=a-b"). Each read is answered from 1 MB
// chunks held in Cache Storage; missing chunks are fetched from the server
// once and stored, so the next visit reads them locally. Everything else
// (the page, crysis2.js, size checks) goes to the network as usual; the page
// caches crysis2.wasm itself.
const DISC_CACHE = 'crysis-disc-v1';
const CHUNK = 1 << 20;
const MEMORY_CHUNKS = 64; // most recently used chunks also kept in memory (small reads repeat a lot)

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));

const stats = { reads: 0, chunkHits: 0, chunkMisses: 0, bytesFromCache: 0, bytesFromServer: 0, errors: 0 };
let cachePromise = null;
const openCache = () => (cachePromise ||= caches.open(DISC_CACHE));
const recent = new Map();   // chunk key -> ArrayBuffer, least recently used first
const inflight = new Map(); // chunk key -> Promise<ArrayBuffer>
const sizes = new Map();    // path -> file size

function remember(key, buf) {
  recent.delete(key);
  recent.set(key, buf);
  if (recent.size > MEMORY_CHUNKS) recent.delete(recent.keys().next().value);
}

async function fileSize(path) {
  if (sizes.has(path)) return sizes.get(path);
  const cache = await openCache();
  const key = '/__crysis/disc-size' + path;
  const hit = await cache.match(key);
  let size;
  if (hit) size = Number(await hit.text());
  else {
    const r = await fetch(path, { method: 'HEAD' });
    if (!r.ok) throw new Error('HEAD ' + path + ': ' + r.status);
    size = Number(r.headers.get('Content-Length'));
    cache.put(key, new Response(String(size))).catch(() => {});
  }
  sizes.set(path, size);
  return size;
}

function chunk(path, index, size) {
  const key = '/__crysis/disc-chunk' + path + '?c=' + index;
  const mem = recent.get(key);
  if (mem) { remember(key, mem); stats.chunkHits++; return Promise.resolve(mem); }
  if (inflight.has(key)) return inflight.get(key);
  const p = (async () => {
    const cache = await openCache();
    const hit = await cache.match(key);
    if (hit) {
      const buf = await hit.arrayBuffer();
      stats.chunkHits++;
      stats.bytesFromCache += buf.byteLength;
      remember(key, buf);
      return buf;
    }
    const start = index * CHUNK, end = Math.min(size, start + CHUNK) - 1;
    const r = await fetch(path, { headers: { Range: `bytes=${start}-${end}` } });
    if (r.status !== 206 && !(r.status === 200 && start === 0 && end === size - 1))
      throw new Error('range ' + path + ': ' + r.status);
    const buf = await r.arrayBuffer();
    stats.chunkMisses++;
    stats.bytesFromServer += buf.byteLength;
    remember(key, buf);
    cache.put(key, new Response(buf)).catch(() => {}); // storage full: just not kept
    return buf;
  })();
  inflight.set(key, p);
  p.then(() => inflight.delete(key), () => inflight.delete(key));
  return p;
}

async function readRange(path, header) {
  const m = /^bytes=(\d+)-(\d*)$/.exec(header.trim());
  if (!m) throw new Error('unsupported range ' + header);
  const size = await fileSize(path);
  const start = Number(m[1]);
  const end = Math.min(m[2] ? Number(m[2]) : size - 1, size - 1);
  if (start > end)
    return new Response(null, { status: 416, headers: { 'Content-Range': `bytes */${size}` } });
  const first = Math.floor(start / CHUNK), last = Math.floor(end / CHUNK);
  const parts = await Promise.all(Array.from({ length: last - first + 1 }, (_, i) => chunk(path, first + i, size)));
  const out = new Uint8Array(end - start + 1);
  let pos = 0;
  parts.forEach((buf, i) => {
    const base = (first + i) * CHUNK;
    const from = Math.max(start, base) - base, to = Math.min(end + 1, base + buf.byteLength) - base;
    out.set(new Uint8Array(buf, from, to - from), pos);
    pos += to - from;
  });
  stats.reads++;
  return new Response(out, {
    status: 206,
    headers: {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(out.length),
      'Content-Range': `bytes ${start}-${end}/${size}`,
      'Accept-Ranges': 'bytes',
      'Cross-Origin-Resource-Policy': 'same-origin',
    },
  });
}

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin || !url.pathname.startsWith('/disc/')) return;
  const range = req.headers.get('Range');
  if (!range) return;
  event.respondWith(readRange(url.pathname, range).catch((e) => {
    stats.errors++;
    console.warn('disc cache:', e);
    return fetch(req); // fall back to the server
  }));
});

self.addEventListener('message', (e) => {
  if (e.data === 'stats' && e.source) e.source.postMessage({ type: 'crysis-disc-stats', stats: { ...stats, memoryChunks: recent.size } });
  if (e.data === 'forget') { recent.clear(); sizes.clear(); cachePromise = null; } // the page cleared the caches
});
