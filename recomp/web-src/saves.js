// Saved games, kept in this browser.
//
// The game keeps its checkpoints and profile the way it would on the console's
// storage device: as files under _content/ in the runtime's writable overlay
// (OVERLAY_DIR, default "overlay"). In the browser that is Emscripten's
// in-memory file system, which a reload empties. This copies those files to
// IndexedDB and back:
// - before the game starts (Module.preRun, holding the start with a run
//   dependency), every stored file is written back into the overlay;
// - while it runs, files that changed are stored once they have stayed the
//   same for one poll (a save is written in pieces), and deleted ones are
//   removed; on leaving the page everything that changed is stored at once.
// Nothing is copied while the game is writing a saved-game file (the runtime's
// crysis_saves_writing / crysis_saves_changes): half a checkpoint is a file
// the game can't load.
// The checkpoint a newer one replaces is kept too (store "previous"). If the
// game can't load a checkpoint (crysis_checkpoint_failures), Resume Game would
// stay on the loading screen forever; a dialog offers the earlier checkpoint,
// or deleting the damaged one.
(() => {
  const DB = 'crysis-saves', STORE = 'files', PREVIOUS = 'previous', POLL_MS = 2000;
  const state = { files: 0, bytes: 0, lastSaved: 0, error: '' };
  const saved = new Map();   // path under the overlay -> signature stored
  const pending = new Map(); // path -> signature seen once, not stored yet
  let started = false, busy = false;
  let frozen = false;        // about to reload with changed stored saves: store nothing more
  let failuresSeen = 0;      // checkpoint load failures already reported

  // The runtime's view of saved-game writes and loads (0 before it is ready).
  const call = (name) => { try { const f = typeof Module !== 'undefined' && Module[name]; return f ? f() : 0; } catch (e) { return 0; } };
  const gameWriting = () => call('_crysis_saves_writing') > 0;
  const gameChanges = () => call('_crysis_saves_changes');
  const isCheckpoint = (p) => /\.csf$/i.test(p);

  let dbPromise = null;
  function openDb() {
    return dbPromise ||= new Promise((resolve, reject) => {
      const req = indexedDB.open(DB, 2); // 2: + "previous"
      req.onupgradeneeded = () => {
        for (const name of [STORE, PREVIOUS])
          if (!req.result.objectStoreNames.contains(name)) req.result.createObjectStore(name);
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }
  // One transaction over `names`; `fn(...stores)` queues the requests. Resolves when it commits.
  async function transact(mode, fn, names = [STORE]) {
    const db = await openDb();
    return new Promise((resolve, reject) => {
      const t = db.transaction(names, mode);
      const result = fn(...names.map((n) => t.objectStore(n)));
      t.oncomplete = () => resolve(result);
      t.onerror = t.onabort = () => reject(t.error);
    });
  }
  function readAll(name = STORE) {
    return transact('readonly', (store) => {
      const out = [];
      store.openCursor().onsuccess = (e) => {
        const c = e.target.result;
        if (c) { out.push([c.key, c.value]); c.continue(); }
      };
      return out;
    }, [name]);
  }

  const overlay = () => { const d = (typeof ENV !== 'undefined' && ENV.OVERLAY_DIR) || 'overlay'; return d.startsWith('/') ? d : '/' + d; };
  const signature = (st) => st.size + ':' + new Date(st.mtime).getTime();

  // Every file under <overlay>/_content: path (relative to the overlay) -> signature.
  function scan() {
    const root = overlay(), found = new Map();
    const walk = (dir) => {
      let names;
      try { names = FS.readdir(root + '/' + dir); } catch (e) { return; }
      for (const n of names) {
        if (n === '.' || n === '..') continue;
        const rel = dir + '/' + n;
        let st;
        try { st = FS.stat(root + '/' + rel); } catch (e) { continue; }
        if (FS.isDir(st.mode)) walk(rel); else found.set(rel, signature(st));
      }
    };
    walk('_content');
    return found;
  }

  function updateTotals(entries) {
    state.files = entries.length;
    state.bytes = entries.reduce((n, [, v]) => n + v.data.byteLength, 0);
  }

  // Store `paths` (current contents) and delete `gone`, in one transaction.
  // Nothing is stored if the game changed a saved-game file since `changes`.
  async function store(paths, gone, current, changes) {
    const root = overlay(), records = [];
    for (const p of paths) {
      // (A copy in a plain buffer: IndexedDB can't store views of shared memory.)
      try { records.push([p, { data: new Uint8Array(FS.readFile(root + '/' + p)), saved: Date.now() }]); } catch (e) { gone.push(p); }
    }
    if (gameWriting() || gameChanges() !== changes) { pending.clear(); return; } // the copies may be half-written
    await transact('readwrite', (s, prev) => {
      for (const [p, v] of records) {
        // Keep the checkpoint this one replaces (read before the put below).
        if (isCheckpoint(p)) s.get(p).onsuccess = (e) => { if (e.target.result) prev.put(e.target.result, p); };
        s.put(v, p);
      }
      for (const p of gone) s.delete(p);
    }, [STORE, PREVIOUS]);
    for (const [p] of records) { saved.set(p, current.get(p)); pending.delete(p); }
    for (const p of gone) { saved.delete(p); pending.delete(p); }
    state.lastSaved = Date.now();
    updateTotals(await readAll());
  }

  // `now`: store everything that changed, without waiting for it to settle.
  async function sync(now = false) {
    if (!started || busy || frozen) return;
    busy = true;
    try {
      // A save is being written: keep the stored copies until it is complete.
      if (gameWriting()) { pending.clear(); return; }
      const changes = gameChanges();
      const current = scan(), changed = [], gone = [];
      for (const [p, sig] of current) {
        if (saved.get(p) === sig) { pending.delete(p); continue; }
        if (now || pending.get(p) === sig) changed.push(p); else pending.set(p, sig);
      }
      for (const p of saved.keys()) if (!current.has(p)) gone.push(p);
      if (changed.length || gone.length) await store(changed, gone, current, changes);
      state.error = '';
    } catch (e) {
      state.error = String(e && e.message || e);
    } finally { busy = false; }
  }

  // Module.preRun: put the stored files back before the game starts.
  function restore() {
    addRunDependency('crysis-saves');
    readAll().then((entries) => {
      const root = overlay();
      for (const [p, v] of entries) {
        const path = root + '/' + p;
        try {
          FS.mkdirTree(path.slice(0, path.lastIndexOf('/')));
          FS.writeFile(path, new Uint8Array(v.data));
          saved.set(p, signature(FS.stat(path)));
        } catch (e) { console.warn('saved game file not restored:', p, e); }
      }
      updateTotals(entries);
      state.lastSaved = entries.reduce((t, [, v]) => Math.max(t, v.saved || 0), 0);
      if (entries.length) console.info(`Saved games: restored ${entries.length} files (${(state.bytes / 1024).toFixed(0)} KB) from this browser`);
    }).catch((e) => {
      state.error = String(e && e.message || e);
      console.warn('saved games not available:', e); // the game still runs, without them
    }).finally(() => {
      started = true;
      setInterval(() => { sync(); checkLoads(); }, POLL_MS);
      removeRunDependency('crysis-saves');
    });
  }

  // Delete the stored saves and the game's copies (takes effect for the next load;
  // anything the game saves after this is kept again).
  async function clear() {
    await transact('readwrite', (s, prev) => { s.clear(); prev.clear(); }, [STORE, PREVIOUS]);
    const root = overlay();
    for (const p of saved.keys()) { try { FS.unlink(root + '/' + p); } catch (e) {} }
    saved.clear(); pending.clear();
    updateTotals([]); state.lastSaved = 0;
  }

  // Settings: delete the checkpoint (and the earlier copy kept with it), or
  // everything stored (checkpoints and the profile with its settings and
  // campaign progress). The running game has read its saves already, so the
  // page reloads; nothing more is stored before that.
  async function deleteStored(onlyCheckpoints) {
    frozen = true;
    try {
      await transact('readwrite', (s, prev) => {
        for (const st of [s, prev]) {
          if (!onlyCheckpoints) { st.clear(); continue; }
          st.openCursor().onsuccess = (e) => {
            const c = e.target.result;
            if (c) { if (isCheckpoint(c.key)) c.delete(); c.continue(); }
          };
        }
      }, [STORE, PREVIOUS]);
    } catch (e) { frozen = false; throw e; }
    location.reload();
  }

  // ---- A checkpoint the game can't load ---------------------------------------
  // The newest stored checkpoint and the one it replaced: { path, saved } or null.
  async function checkpoints() {
    const pick = (entries) => {
      const c = entries.filter(([p]) => isCheckpoint(p)).sort((a, b) => (b[1].saved || 0) - (a[1].saved || 0))[0];
      return c ? { path: c[0], saved: c[1].saved || 0 } : null;
    };
    return { current: pick(await readAll(STORE)), previous: pick(await readAll(PREVIOUS)) };
  }

  // Put the earlier checkpoint back (the damaged one is dropped), or just drop
  // the damaged one; then reload, since the game has read its saves already.
  async function usePrevious(path) {
    frozen = true;
    await transact('readwrite', (s, prev) => {
      prev.get(path).onsuccess = (e) => { if (e.target.result) { s.put(e.target.result, path); prev.delete(path); } };
    }, [STORE, PREVIOUS]);
    location.reload();
  }
  async function dropCheckpoint(path) {
    frozen = true;
    await transact('readwrite', (s) => { s.delete(path); });
    location.reload();
  }

  function checkLoads() {
    const n = call('_crysis_checkpoint_failures');
    if (n > failuresSeen) { failuresSeen = n; showLoadFailure().catch((e) => console.warn('saved games:', e)); }
  }

  const css = `
  #savefail { position: fixed; inset: 0; z-index: 21; display: flex; align-items: center; justify-content: center;
    background: rgba(5,7,6,.72); padding: 16px; }
  #savefail .panel { width: min(480px, 100%); background: var(--panel); border: 1px solid var(--line); border-radius: 12px;
    padding: 18px 20px 20px; color: var(--text); }
  #savefail h2 { margin: 0 0 10px; font-size: 17px; font-weight: 600; }
  #savefail p { margin: 0 0 10px; color: var(--dim); font-size: 13px; }
  #savefail .foot { display: flex; justify-content: flex-end; gap: 10px; flex-wrap: wrap; margin-top: 16px; }
  #savefail .btn { background: #1d2321; color: var(--text); border: 1px solid var(--line); border-radius: 6px; padding: 6px 12px;
    font: inherit; cursor: pointer; }
  #savefail .btn:hover { border-color: var(--accent-dim); }
  #savefail .btn.primary { background: var(--accent-dim); border-color: var(--accent-dim); }
  #savefail .btn:disabled { opacity: .5; cursor: default; }
  `;

  async function showLoadFailure() {
    if (document.getElementById('savefail')) return;
    const { current, previous } = await checkpoints();
    const when = (t) => t ? new Date(t).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) : 'earlier';
    const text = [], buttons = [];
    if (current) {
      text.push(`The game can't load the last checkpoint (stored ${when(current.saved)}): the file is damaged or incomplete, so Resume Game would stay on the loading screen.`);
      if (previous) {
        text.push(`The checkpoint before it (stored ${when(previous.saved)}) is still in this browser. Loading it reloads the page; then choose Resume Game.`);
        buttons.push(['Delete this checkpoint', () => dropCheckpoint(current.path)], ['Load the earlier checkpoint', () => usePrevious(previous.path), true]);
      } else {
        text.push('Deleting it keeps your profile, settings and campaign progress, and reloads the page. Then open Campaign and choose Replay Mission to play the mission again.');
        buttons.push(['Close', null], ['Delete this checkpoint', () => dropCheckpoint(current.path), true]);
      }
    } else {
      text.push("The game can't load its last checkpoint, and no copy of it is stored in this browser, so Resume Game would stay on the loading screen. Reload the page, then open Campaign and choose Replay Mission or New Campaign.");
      buttons.push(['Close', null], ['Reload', () => location.reload(), true]);
    }
    if (!document.getElementById('savefail-css')) {
      const style = document.createElement('style');
      style.id = 'savefail-css'; style.textContent = css;
      document.head.appendChild(style);
    }
    const root = document.createElement('div');
    root.id = 'savefail';
    root.innerHTML = '<div class="panel" role="alertdialog" aria-labelledby="savefail-title"><h2 id="savefail-title"></h2><div class="text"></div><div class="foot"></div></div>';
    root.querySelector('h2').textContent = current ? 'This checkpoint can\'t be loaded' : 'No checkpoint to resume';
    for (const t of text) { const p = document.createElement('p'); p.textContent = t; root.querySelector('.text').appendChild(p); }
    let primary = null;
    for (const [label, act, isPrimary] of buttons) {
      const b = document.createElement('button');
      b.type = 'button'; b.className = 'btn' + (isPrimary ? ' primary' : ''); b.textContent = label;
      b.addEventListener('click', () => {
        if (!act) { root.remove(); return; }
        for (const x of root.querySelectorAll('button')) x.disabled = true;
        Promise.resolve(act()).catch((e) => { console.warn('saved games:', e); root.remove(); });
      });
      root.querySelector('.foot').appendChild(b);
      if (isPrimary) primary = b;
    }
    if (document.pointerLockElement) document.exitPointerLock();
    document.body.appendChild(root);
    if (primary) primary.focus();
  }

  addEventListener('pagehide', () => { sync(true); });
  addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') sync(true); });

  window.crysisSaves = {
    preRun: restore,
    info: () => ({ ...state }),
    flush: () => sync(true),
    clear,
    deleteCheckpoint: () => deleteStored(true), // then reloads
    deleteAll: () => deleteStored(false),       // then reloads
    checkpoints,      // { current, previous }: the stored checkpoints
    showLoadFailure,  // the dialog shown when the game can't load a checkpoint
  };
})();
