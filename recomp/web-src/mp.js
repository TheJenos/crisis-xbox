// Multiplayer link (runtime/mp.cpp, net.cpp): session listings and the game's
// packets between players.
//
// Two ways:
// - "local": tabs of this browser, over a BroadcastChannel (testing on one PC).
// - "online": Firebase Realtime Database for the lobby (listed sessions,
//   players) and for setting up connections, and WebRTC data channels for the
//   packets (unordered, no retransmits: like the console's UDP).
//
// Firebase is used through its REST API (live updates over EventSource): no
// SDK to load, which a cross-origin isolated page couldn't anyway. Its config
// comes from firebase-config.json next to the page, kept out of the repository.
//
// Each player (each tab) has a 24-bit ID; the game's address for them is
// 10.x.y.z from its bytes (mp.cpp), so a packet's destination says which
// player it's for.
(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  let settings = {};
  try { settings = JSON.parse(localStorage.getItem('crysisMp') || '{}') || {}; } catch (e) {}
  const forced = params.get('mp'); // local | online | off (testing)
  const mode = forced || (settings.enabled ? (settings.link === 'local' ? 'local' : 'online') : 'off');
  const enabled = mode === 'local' || mode === 'online';
  const log = (...a) => (window.log ? window.log('[mp] ' + a.join(' ')) : console.log('[mp]', ...a));

  // This tab's ID: kept across reloads of the tab, new in another tab.
  const newId = () => {
    for (;;) {
      const x = crypto.getRandomValues(new Uint32Array(1))[0] & 0xffffff;
      if (x && (x & 0xff) !== 0 && (x & 0xff) !== 0xff) return x;
    }
  };
  let id = 0;
  try { id = parseInt(sessionStorage.getItem('crysisMpId') || '0', 16) || 0; } catch (e) {}
  if (!id) { id = newId(); try { sessionStorage.setItem('crysisMpId', id.toString(16)); } catch (e) {} }
  const hex6 = (n) => n.toString(16).padStart(6, '0');
  const idHex = hex6(id);
  const ipOf = (pid) => (0x0a000000 | pid) >>> 0;
  const idOf = (ip) => ip & 0xffffff;
  // Gamertag: letters, digits and spaces, up to 15 (the console's rules).
  const cleanName = (s) => String(s || '').replace(/[^A-Za-z0-9 ]/g, '').trim().slice(0, 15);
  const name = cleanName(params.get('mpname') || settings.name) || 'Player' + idHex.slice(-4).toUpperCase();

  // ---- The game side ------------------------------------------------------
  let game = null; // Module, once attached
  const mine = new Map(); // key -> listing line (sessions this player hosts)
  const listed = new Map(); // key -> { line, host, seen } (other players' sessions)
  let status = 'menus';

  function pushDirectory() {
    if (!game || !game._crysis_mp_set_directory || !globalThis.wasmMemory) return;
    const now = Date.now();
    const lines = [];
    for (const [key, s] of listed) {
      if (now - s.seen > 15000 || mine.has(key)) { listed.delete(key); continue; }
      lines.push(s.line);
    }
    const bytes = new TextEncoder().encode(lines.join('\n') + '\0');
    const ptr = game._malloc(bytes.length);
    new Uint8Array(wasmMemory.buffer, ptr >>> 0, bytes.length).set(bytes);
    game._crysis_mp_set_directory(ptr);
    game._free(ptr);
  }

  function deliver(fromId, fromPort, toPort, bytes) {
    if (!game || !game._crysis_net_receive || !globalThis.wasmMemory) return;
    stats.recv++;
    const ptr = game._malloc(bytes.length || 1);
    new Uint8Array(wasmMemory.buffer, ptr >>> 0, bytes.length).set(bytes);
    game._crysis_net_receive(ipOf(fromId), fromPort, toPort, ptr, bytes.length);
    game._free(ptr);
  }

  const stats = { sent: 0, recv: 0 };

  // ---- Between tabs: BroadcastChannel ---------------------------------------
  function localLink() {
    const ch = new BroadcastChannel('crysis2-mp');
    ch.onmessage = (e) => {
      const m = e.data;
      if (!m || m.from === id) return;
      if (m.k === 's') listed.set(m.key, { line: m.line, host: m.from, seen: Date.now() });
      else if (m.k === 'w') listed.delete(m.key);
      else if (m.k === 'p' && m.to === id) deliver(m.from, m.fp, m.tp, m.d);
      if (m.k === 's' || m.k === 'w') pushDirectory();
    };
    setInterval(() => { for (const [key, line] of mine) ch.postMessage({ k: 's', from: id, key, line }); pushDirectory(); }, 2000);
    return {
      announce: (key, line) => ch.postMessage({ k: 's', from: id, key, line }),
      withdraw: (key) => ch.postMessage({ k: 'w', from: id, key }),
      send: (to, fp, tp, d) => ch.postMessage({ k: 'p', from: id, to, fp, tp, d }),
      leave: () => { for (const key of mine.keys()) ch.postMessage({ k: 'w', from: id, key }); },
    };
  }

  // ---- Between machines: Firebase + WebRTC ----------------------------------
  function onlineLink(config) {
    const base = config.databaseURL.replace(/\/$/, '') + '/c2';
    const url = (path) => `${base}/${path}.json`;
    const put = (path, data) => fetch(url(path), { method: 'PUT', body: JSON.stringify(data) }).catch(() => {});
    const del = (path, keepalive) => fetch(url(path), { method: 'DELETE', keepalive: !!keepalive }).catch(() => {});
    const post = (path, data) => fetch(url(path), { method: 'POST', body: JSON.stringify(data) }).catch(() => {});
    let skew = 0; // server time - local time (ms), from our own writes
    const serverNow = () => Date.now() + skew;
    const stamp = { '.sv': 'timestamp' };

    // A live view of a path: Firebase's streaming REST API (EventSource), with
    // reconnects. onChange(path, data) gets each 'put' and 'patch'.
    function watch(path, onChange) {
      let es = null;
      const open = () => {
        es = new EventSource(url(path));
        const handle = (e) => { try { const m = JSON.parse(e.data); onChange(m.path, m.data, e.type); } catch (err) {} };
        es.addEventListener('put', handle);
        es.addEventListener('patch', handle);
        es.addEventListener('auth_revoked', () => { es.close(); setTimeout(open, 2000); });
        es.onerror = () => { if (es.readyState === EventSource.CLOSED) setTimeout(open, 3000); };
      };
      open();
      return () => es && es.close();
    }

    // Listed sessions: c2/sessions/{key} = { host, name, line, t }.
    const sessions = new Map();
    watch('sessions', (path, data, type) => {
      const apply = (key, v) => { if (v && v.line && v.host !== idHex) sessions.set(key, v); else sessions.delete(key); };
      if (path === '/') {
        if (type === 'put') sessions.clear();
        for (const [key, v] of Object.entries(data || {})) apply(key, v);
      } else {
        const parts = path.split('/').filter(Boolean);
        const key = parts[0];
        if (parts.length === 1) apply(key, data);
        else if (sessions.has(key)) { const v = { ...sessions.get(key), [parts[1]]: data }; apply(key, v); }
      }
      refreshListed();
    });
    function refreshListed() {
      const now = serverNow();
      listed.clear();
      for (const [key, v] of sessions)
        if (!(typeof v.t === 'number' && now - v.t > 20000)) listed.set(key, { line: v.line, host: parseInt(v.host, 16), seen: Date.now() });
      pushDirectory();
    }

    // This player and its sessions, kept fresh (stale ones are ignored after 20 s).
    const putPlayer = () =>
      fetch(url(`players/${idHex}`), { method: 'PUT', body: JSON.stringify({ name, status, t: stamp }) })
        .then((r) => r.json()).then((v) => { if (v && typeof v.t === 'number') skew = v.t - Date.now(); }).catch(() => {});
    const beat = () => {
      putPlayer();
      for (const [key, line] of mine) put(`sessions/${key}`, { host: idHex, name, line, t: stamp });
      refreshListed();
    };
    setInterval(beat, 5000);
    beat();

    // Connections: one RTCPeerConnection per player, its data channel agreed on
    // both sides (negotiated, id 0). The player with the lower ID makes the
    // offer; the other asks for one ('hello'). Signals go through
    // c2/signal/{to}/{push id} and are deleted once read.
    const peers = new Map(); // id -> { pc, dc, queue, remoteSet, pending }
    const ice = { iceServers: [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun1.l.google.com:19302' }] };
    const signal = (to, msg) => post(`signal/${hex6(to)}`, { from: idHex, ...msg });

    function peer(pid) {
      let p = peers.get(pid);
      if (p && !['failed', 'closed'].includes(p.pc.connectionState)) return p;
      if (p) try { p.pc.close(); } catch (e) {}
      const pc = new RTCPeerConnection(ice);
      const dc = pc.createDataChannel('game', { negotiated: true, id: 0, ordered: false, maxRetransmits: 0 });
      dc.binaryType = 'arraybuffer';
      p = { pc, dc, queue: [], remoteSet: false, pending: [] };
      peers.set(pid, p);
      dc.onopen = () => { log(`connected to player ${hex6(pid)}`); for (const b of p.queue) dc.send(b); p.queue = []; };
      dc.onmessage = (e) => {
        const v = new DataView(e.data);
        deliver(pid, v.getUint16(0), v.getUint16(2), new Uint8Array(e.data, 4));
      };
      pc.onicecandidate = (e) => { if (e.candidate) signal(pid, { type: 'cand', cand: JSON.stringify(e.candidate) }); };
      pc.onconnectionstatechange = () => { if (pc.connectionState === 'failed') log(`connection to player ${hex6(pid)} failed`); };
      if (id < pid) offer(pid, p);
      else signal(pid, { type: 'hello' });
      return p;
    }
    async function offer(pid, p) {
      await p.pc.setLocalDescription(await p.pc.createOffer());
      signal(pid, { type: 'offer', sdp: p.pc.localDescription.sdp });
    }
    async function onSignal(m) {
      const pid = parseInt(m.from, 16);
      if (!pid || pid === id) return;
      if (m.type === 'hello') {
        const old = peers.get(pid);
        if (old) { try { old.pc.close(); } catch (e) {} peers.delete(pid); }
        peer(pid).queue.push(...(old ? old.queue : []));
        return;
      }
      let p = peers.get(pid);
      if (m.type === 'offer') {
        const queued = p ? p.queue : [];
        if (p) { try { p.pc.close(); } catch (e) {} peers.delete(pid); }
        // (the answering side: build the connection without making an offer)
        const pc = new RTCPeerConnection(ice);
        const dc = pc.createDataChannel('game', { negotiated: true, id: 0, ordered: false, maxRetransmits: 0 });
        dc.binaryType = 'arraybuffer';
        p = { pc, dc, queue: queued, remoteSet: false, pending: [] };
        peers.set(pid, p);
        dc.onopen = () => { log(`connected to player ${hex6(pid)}`); for (const b of p.queue) dc.send(b); p.queue = []; };
        dc.onmessage = (e) => { const v = new DataView(e.data); deliver(pid, v.getUint16(0), v.getUint16(2), new Uint8Array(e.data, 4)); };
        pc.onicecandidate = (e) => { if (e.candidate) signal(pid, { type: 'cand', cand: JSON.stringify(e.candidate) }); };
        await pc.setRemoteDescription({ type: 'offer', sdp: m.sdp });
        p.remoteSet = true;
        for (const c of p.pending) pc.addIceCandidate(c).catch(() => {});
        p.pending = [];
        await pc.setLocalDescription(await pc.createAnswer());
        signal(pid, { type: 'answer', sdp: pc.localDescription.sdp });
      } else if (m.type === 'answer' && p) {
        await p.pc.setRemoteDescription({ type: 'answer', sdp: m.sdp });
        p.remoteSet = true;
        for (const c of p.pending) p.pc.addIceCandidate(c).catch(() => {});
        p.pending = [];
      } else if (m.type === 'cand' && p) {
        const c = JSON.parse(m.cand);
        if (p.remoteSet) p.pc.addIceCandidate(c).catch(() => {});
        else p.pending.push(c);
      }
    }
    const seenSignals = new Set();
    watch(`signal/${idHex}`, (path, data) => {
      const items = path === '/' ? Object.entries(data || {}) : [[path.split('/').filter(Boolean)[0], data]];
      for (const [key, m] of items) {
        if (!m || seenSignals.has(key)) continue;
        seenSignals.add(key);
        del(`signal/${idHex}/${key}`);
        onSignal(m).catch((e) => log('signal error: ' + e));
      }
    });

    return {
      announce: (key, line) => put(`sessions/${key}`, { host: idHex, name, line, t: stamp }),
      withdraw: (key) => del(`sessions/${key}`),
      send: (to, fp, tp, d) => {
        const p = peer(to);
        const b = new Uint8Array(4 + d.length);
        new DataView(b.buffer).setUint16(0, fp);
        new DataView(b.buffer).setUint16(2, tp);
        b.set(d, 4);
        if (p.dc.readyState === 'open') p.dc.send(b.buffer);
        else if (p.queue.length < 256) p.queue.push(b.buffer);
      },
      leave: () => {
        for (const key of mine.keys()) del(`sessions/${key}`, true);
        del(`players/${idHex}`, true);
        for (const p of peers.values()) try { p.pc.close(); } catch (e) {}
      },
      setStatus: () => putPlayer(),
    };
  }

  // ---- Start ------------------------------------------------------------------
  let link = null;
  async function start() {
    if (mode === 'online') {
      try {
        const r = await fetch('firebase-config.json', { cache: 'no-store' });
        const config = r.ok ? await r.json() : null;
        if (config && config.databaseURL) {
          link = onlineLink(config);
          log(`online as ${name} (player ${idHex}, address ${[10, id >> 16 & 255, id >> 8 & 255, id & 255].join('.')})`);
          return;
        }
      } catch (e) {}
      log('no Firebase config (firebase-config.json): multiplayer links only tabs of this browser');
    }
    link = localLink();
    log(`between tabs as ${name} (player ${idHex})`);
  }

  window.crysisMp = {
    enabled, mode, id: idHex, name,
    // Runtime environment for the game (index.html preRun).
    env: () => ({ LIVE: '1', MP_ID: idHex, MP_NAME: name }),
    attach(Module) {
      if (!enabled) return;
      game = Module;
      Module.mpSend = (fromIp, fromPort, toIp, toPort, bytes) => {
        stats.sent++;
        if (link) link.send(idOf(toIp), fromPort, toPort, bytes);
      };
      Module.mpPublish = (line) => {
        const key = line.split(' ')[0];
        mine.set(key, line);
        if (link) link.announce(key, line);
      };
      Module.mpUnpublish = (key) => {
        mine.delete(key);
        if (link) link.withdraw(key);
      };
      start();
      setInterval(pushDirectory, 2000);
      addEventListener('pagehide', () => link && link.leave());
    },
    stats: () => ({ ...stats, listed: listed.size, hosting: mine.size }),
    // Called by the game (MAIN_THREAD_ASYNC_EM_ASM). Such calls can run inside
    // the page thread's wait for a lock (Emscripten handles queued calls while
    // it waits), so nothing here may call back into the game: the update runs
    // from the event loop, and it only writes this player's record.
    setStatus(s) { status = s; setTimeout(() => { if (link && link.setStatus) link.setStatus(); }, 0); },
  };
})();
