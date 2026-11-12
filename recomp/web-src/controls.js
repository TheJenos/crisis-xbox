// Keyboard and mouse controls for the browser build: rebindable bindings
// (saved in this browser), mouse look through pointer lock, and the settings
// panel (gear button, or F1). The game only sees an Xbox 360 pad, so every
// binding presses and releases a pad control by number (runtime/input.h,
// InputControl). Inputs are named "Key:<KeyboardEvent.code>",
// "Mouse:<button>" and "Wheel:Up" / "Wheel:Down".
(() => {
  'use strict';
  const call = (fn, ...args) => { const f = window.Module && window.Module[fn]; if (f) f(...args); };
  const isTouch = matchMedia('(pointer: coarse)').matches || ('ontouchstart' in window && navigator.maxTouchPoints > 0);

  // ---- Pad controls and defaults ------------------------------------------
  // id: control number in runtime/input.h. hint: what it usually does in game.
  const CONTROLS = [
    { group: 'Move and look' },
    { id: 16, pad: 'Left stick up', hint: 'move forward' },
    { id: 17, pad: 'Left stick down', hint: 'move back' },
    { id: 18, pad: 'Left stick left', hint: 'strafe left' },
    { id: 19, pad: 'Left stick right', hint: 'strafe right' },
    { id: 20, pad: 'Right stick up', hint: 'look up (the mouse also looks)' },
    { id: 21, pad: 'Right stick down', hint: 'look down' },
    { id: 22, pad: 'Right stick left', hint: 'look left' },
    { id: 23, pad: 'Right stick right', hint: 'look right' },
    { group: 'Actions' },
    { id: 15, pad: 'RT', hint: 'fire' },
    { id: 14, pad: 'LT', hint: 'aim' },
    { id: 0, pad: 'A', hint: 'jump · select' },
    { id: 1, pad: 'B', hint: 'crouch · back' },
    { id: 2, pad: 'X', hint: 'reload · use' },
    { id: 3, pad: 'Y', hint: 'switch weapon' },
    { id: 8, pad: 'Left stick click', hint: 'sprint' },
    { id: 9, pad: 'Right stick click' },
    { id: 4, pad: 'LB' },
    { id: 5, pad: 'RB' },
    { group: 'Menus and D-pad' },
    { id: 7, pad: 'START', hint: 'pause menu' },
    { id: 6, pad: 'BACK' },
    { id: 10, pad: 'D-pad up' },
    { id: 11, pad: 'D-pad down' },
    { id: 12, pad: 'D-pad left' },
    { id: 13, pad: 'D-pad right' },
  ];
  const DEFAULTS = {
    16: ['Key:KeyW'], 17: ['Key:KeyS'], 18: ['Key:KeyA'], 19: ['Key:KeyD'],
    20: ['Key:KeyI'], 21: ['Key:KeyK'], 22: ['Key:KeyJ'], 23: ['Key:KeyL'],
    15: ['Mouse:0', 'Key:ControlLeft', 'Key:ControlRight'], 14: ['Mouse:2', 'Key:ShiftLeft', 'Key:ShiftRight'],
    0: ['Key:Space'], 1: ['Key:Escape'], 2: ['Key:KeyR'], 3: ['Key:KeyF', 'Wheel:Up', 'Wheel:Down'],
    8: ['Key:KeyC'], 9: ['Key:KeyV', 'Mouse:1'], 4: ['Key:KeyQ', 'Mouse:3'], 5: ['Key:KeyE', 'Mouse:4'],
    7: ['Key:Enter', 'Key:NumpadEnter'], 6: ['Key:Tab', 'Key:Backspace'],
    10: ['Key:ArrowUp'], 11: ['Key:ArrowDown'], 12: ['Key:ArrowLeft'], 13: ['Key:ArrowRight'],
  };
  const clone = (b) => Object.fromEntries(Object.entries(b).map(([k, v]) => [k, [...v]]));

  // ---- Saved settings -----------------------------------------------------
  const load = (key, fallback) => { try { const v = JSON.parse(localStorage.getItem(key)); return v ?? fallback; } catch (e) { return fallback; } };
  const save = (key, value) => { try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) {} };
  let bindings = clone(DEFAULTS);
  const stored = load('crysisBindings', null);
  if (stored && stored.version === 1 && stored.bindings) bindings = clone(stored.bindings);
  const mouse = Object.assign({ sensitivity: 1, invertY: false }, load('crysisMouse', {}));
  let bindingsVersion = 0; // bumped on every change (the prompt hints redraw)
  const saveBindings = () => { bindingsVersion++; save('crysisBindings', { version: 1, bindings }); };
  const applyMouse = () => call('_crysis_mouse_config', Number(mouse.sensitivity) || 1, mouse.invertY ? 1 : 0);

  // input name -> control ids
  let lookup = new Map();
  function rebuildLookup() {
    lookup = new Map();
    for (const [id, inputs] of Object.entries(bindings))
      for (const input of inputs) {
        if (!lookup.has(input)) lookup.set(input, []);
        lookup.get(input).push(Number(id));
      }
  }
  rebuildLookup();

  // ---- Names shown for inputs ---------------------------------------------
  const KEY_NAMES = { Space: 'Space', Enter: 'Enter', NumpadEnter: 'Num Enter', Escape: 'Esc', Tab: 'Tab', Backspace: 'Backspace',
    ShiftLeft: 'Left Shift', ShiftRight: 'Right Shift', ControlLeft: 'Left Ctrl', ControlRight: 'Right Ctrl',
    AltLeft: 'Left Alt', AltRight: 'Right Alt', MetaLeft: 'Left Win', MetaRight: 'Right Win', CapsLock: 'Caps Lock',
    ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→', Backquote: '`', Minus: '-', Equal: '=',
    BracketLeft: '[', BracketRight: ']', Backslash: '\\', Semicolon: ';', Quote: "'", Comma: ',', Period: '.', Slash: '/' };
  const MOUSE_NAMES = ['Left mouse', 'Middle mouse', 'Right mouse', 'Mouse 4', 'Mouse 5'];
  function inputName(input) {
    const [kind, v] = [input.slice(0, input.indexOf(':')), input.slice(input.indexOf(':') + 1)];
    if (kind === 'Mouse') return MOUSE_NAMES[Number(v)] || 'Mouse ' + (Number(v) + 1);
    if (kind === 'Wheel') return v === 'Up' ? 'Wheel up' : 'Wheel down';
    if (KEY_NAMES[v]) return KEY_NAMES[v];
    if (/^Key[A-Z]$/.test(v)) return v.slice(3);
    if (/^Digit\d$/.test(v)) return v.slice(5);
    if (/^Numpad/.test(v)) return 'Num ' + v.slice(6);
    return v;
  }

  // ---- Feeding the game -----------------------------------------------------
  let blocked = false;                 // settings open: the game gets nothing
  const held = new Map();              // control id -> Set of inputs holding it
  function press(input) {
    if (blocked) return false;
    const ids = lookup.get(input);
    if (!ids) return false;
    for (const id of ids) {
      if (!held.has(id)) held.set(id, new Set());
      const s = held.get(id);
      if (!s.size) call('_crysis_control', id, 1);
      s.add(input);
    }
    return true;
  }
  function release(input) {
    const ids = lookup.get(input);
    if (!ids) return false;
    for (const id of ids) {
      const s = held.get(id);
      if (s && s.delete(input) && !s.size) call('_crysis_control', id, 0);
    }
    return true;
  }
  function releaseAll() {
    for (const [id, s] of held) if (s.size) { s.clear(); call('_crysis_control', id, 0); }
  }
  function tap(input) { if (press(input)) setTimeout(() => release(input), 90); }

  const canvas = () => document.getElementById('screen');
  const locked = () => document.pointerLockElement === canvas();
  function capturePointer() {
    const c = canvas();
    // The browser refuses a capture right after Esc released it (about a
    // second); that's not an error worth reporting, the next click works.
    const plain = () => {
      try { const p = c.requestPointerLock(); if (p && p.catch) p.catch(() => {}); } catch (e) {}
    };
    try {
      const p = c.requestPointerLock({ unadjustedMovement: true }); // raw movement, no OS acceleration
      if (p && p.catch) p.catch((e) => { if (!e || e.name !== 'SecurityError') plain(); });
    } catch (e) { plain(); }
  }

  document.addEventListener('keydown', (e) => {
    if (capture) return; // the settings panel is listening for a new binding
    if (e.code === 'F1') { e.preventDefault(); toggleSettings(); return; }
    if (settingsOpen) return;
    const input = 'Key:' + e.code;
    if (e.repeat) { if (lookup.has(input)) e.preventDefault(); return; }
    if (press(input)) e.preventDefault();
  });
  document.addEventListener('keyup', (e) => { if (release('Key:' + e.code)) e.preventDefault(); });
  window.addEventListener('blur', releaseAll); // nothing stays held when the window loses focus

  function setupMouse() {
    const c = canvas();
    c.addEventListener('mousedown', (e) => {
      e.preventDefault();
      if (settingsOpen) return;
      if (!locked()) { // the capturing click isn't a game button
        if (!isTouch) capturePointer();
        // Settings, Display: fullscreen when playing (pointer capture first: it
        // needs the click, and so does fullscreen).
        if ((load('crysisVideo', {}) || {}).autoFullscreen !== false && window.crysisDisplay) crysisDisplay.enterFullscreen();
        return;
      }
      press('Mouse:' + e.button);
    });
    document.addEventListener('mouseup', (e) => release('Mouse:' + e.button));
    document.addEventListener('mousemove', (e) => {
      if (locked() && !blocked && (e.movementX || e.movementY)) call('_crysis_mouse_move', e.movementX, e.movementY);
    });
    let lastWheel = 0;
    c.addEventListener('wheel', (e) => {
      if (!locked()) return;
      e.preventDefault();
      const now = performance.now();
      if (now - lastWheel < 150) return; // one tap per wheel notch burst
      lastWheel = now;
      tap(e.deltaY < 0 ? 'Wheel:Up' : 'Wheel:Down');
    }, { passive: false });
    c.addEventListener('contextmenu', (e) => e.preventDefault());
    document.addEventListener('pointerlockchange', () => {
      document.body.classList.toggle('locked', locked());
      if (!locked()) for (const input of ['Mouse:0', 'Mouse:1', 'Mouse:2', 'Mouse:3', 'Mouse:4']) release(input);
    });
  }

  // ---- Settings panel ---------------------------------------------------------
  const css = `
  #setbtn { position: absolute; top: 72px; right: 8px; z-index: 4; width: 30px; height: 30px; border-radius: 8px;
    background: rgba(11,13,12,.6); border: 1px solid var(--line); color: var(--dim); font-size: 16px; line-height: 1;
    cursor: pointer; opacity: .55; transition: opacity .2s; }
  #setbtn:hover { opacity: 1; color: var(--text); }
  body.touch #setbtn { top: 96px; right: 12px; }
  #settings { position: fixed; inset: 0; z-index: 20; display: none; align-items: center; justify-content: center;
    background: rgba(5,7,6,.72); padding: 24px 16px; }
  #settings.open { display: flex; }
  #settings .panel { width: min(680px, 100%); height: min(640px, 100%); display: flex; flex-direction: column;
    background: var(--panel); border: 1px solid var(--line); border-radius: 12px; color: var(--text); overflow: hidden;
    box-shadow: 0 24px 60px rgba(0,0,0,.55); }
  #settings .head { padding: 16px 20px 0; }
  #settings h2 { margin: 0; font-size: 17px; font-weight: 600; }
  /* Tabs */
  #settings .tabs { display: flex; gap: 4px; padding: 12px 16px 0; border-bottom: 1px solid var(--line); overflow-x: auto;
    scrollbar-width: none; flex: none; }
  #settings .tabs::-webkit-scrollbar { display: none; }
  #settings .tab { position: relative; background: none; border: 0; color: var(--dim); font: inherit; font-size: 13px; font-weight: 600;
    padding: 9px 12px 11px; cursor: pointer; white-space: nowrap; border-radius: 6px 6px 0 0; }
  #settings .tab:hover { color: var(--text); background: rgba(255,255,255,.03); }
  #settings .tab[aria-selected="true"] { color: var(--text); }
  #settings .tab[aria-selected="true"]::after { content: ""; position: absolute; left: 10px; right: 10px; bottom: -1px; height: 2px;
    background: var(--accent); border-radius: 2px; }
  #settings .tab:focus-visible { outline: 2px solid var(--accent-dim); outline-offset: -2px; }
  #settings .pages { flex: 1; min-height: 0; overflow-y: auto; padding: 4px 20px 12px; }
  #settings .page[hidden] { display: none; }
  #settings .page > h3:first-child, #settings .page > #set-controls > h3:first-child { margin-top: 14px; }
  #settings .panel > .foot { flex: none; margin: 0; padding: 12px 20px; border-top: 1px solid var(--line); align-items: center; }
  #settings h3 { margin: 18px 0 6px; font-size: 12px; font-weight: 600; color: var(--dim); text-transform: uppercase; letter-spacing: .06em; }
  #settings .head { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
  #settings .note { color: var(--dim); font-size: 12px; margin: 4px 0 0; }
  #settings .row { display: flex; align-items: center; gap: 10px; padding: 6px 0; border-top: 1px solid #1f2523; }
  #settings .row .what { width: 210px; flex: none; }
  #settings .row .what b { font-weight: 600; font-size: 13px; }
  #settings .row .what span { display: block; color: var(--dim); font-size: 11.5px; }
  #settings .chips { display: flex; flex-wrap: wrap; gap: 6px; flex: 1; align-items: center; }
  #settings .chip { display: inline-flex; align-items: center; gap: 4px; background: #1d2321; border: 1px solid var(--line);
    border-radius: 6px; padding: 2px 4px 2px 8px; font-size: 12.5px; }
  #settings .chip button { background: none; border: 0; color: var(--dim); cursor: pointer; font-size: 13px; padding: 0 3px; }
  #settings .chip button:hover { color: var(--warn); }
  #settings .add { background: none; border: 1px dashed var(--line); color: var(--dim); border-radius: 6px; padding: 2px 8px;
    cursor: pointer; font: inherit; font-size: 12.5px; }
  #settings .add:hover { color: var(--accent); border-color: var(--accent-dim); }
  #settings .add.listening { color: var(--accent); border-color: var(--accent); border-style: solid; }
  #settings .btn { background: #1d2321; color: var(--text); border: 1px solid var(--line); border-radius: 6px; padding: 5px 12px;
    font: inherit; cursor: pointer; }
  #settings .btn:hover { border-color: var(--accent-dim); }
  #settings .btn.primary { background: var(--accent-dim); border-color: var(--accent-dim); }
  #settings .btn:disabled { opacity: .45; cursor: default; border-color: var(--line); }
  #settings .x { background: none; border: 0; color: var(--dim); font-size: 20px; cursor: pointer; line-height: 1; }
  #settings .field { display: flex; align-items: center; gap: 12px; padding: 6px 0; }
  #settings .field label { width: 210px; flex: none; font-size: 13px; }
  #settings input[type=range] { flex: 1; accent-color: var(--accent); }
  #settings input[type=checkbox] { accent-color: var(--accent); width: 16px; height: 16px; }
  #settings select { flex: 1; background: #1d2321; color: var(--text); border: 1px solid var(--line); border-radius: 6px;
    padding: 5px 8px; font: inherit; font-size: 13px; }
  #settings .val { width: 44px; text-align: right; font-variant-numeric: tabular-nums; color: var(--dim); }
  #settings .foot { display: flex; justify-content: space-between; gap: 10px; margin-top: 18px; flex-wrap: wrap; }
  #settings .moved { color: var(--warn); font-size: 12px; min-height: 16px; margin-top: 6px; }
  @media (max-width: 560px) { #settings .row, #settings .field { flex-wrap: wrap; } #settings .row .what, #settings .field label { width: 100%; } }

  /* Keys for the controller buttons the game is prompting for (top centre) */
  #prompthints { position: absolute; top: 6px; left: 50%; transform: translate(-50%, -6px); z-index: 4; display: flex; gap: 6px;
    flex-wrap: wrap; justify-content: center; max-width: calc(100% - 220px); pointer-events: none; opacity: 0;
    transition: opacity .18s ease, transform .18s ease; }
  #prompthints.shown { opacity: 1; transform: translate(-50%, 0); }
  #prompthints .hint { display: inline-flex; align-items: center; gap: 7px; background: rgba(11,13,12,.8); border: 1px solid var(--line);
    border-radius: 99px; padding: 3px 10px 3px 4px; font-size: 12.5px; color: var(--text); white-space: nowrap;
    box-shadow: 0 4px 14px rgba(0,0,0,.35); }
  #prompthints .pad { min-width: 22px; height: 22px; border-radius: 11px; display: inline-flex; align-items: center; justify-content: center;
    font: 700 11.5px/1 system-ui, sans-serif; color: #10140f; background: #c9d1cd; padding: 0 6px; }
  #prompthints .pad.a { background: #6cc24a; } #prompthints .pad.b { background: #e5483d; color: #fff; }
  #prompthints .pad.x { background: #3d8fe0; color: #fff; } #prompthints .pad.y { background: #f2c230; }
  #prompthints .key { display: inline-block; background: #232a28; border: 1px solid #3a4441; border-bottom-width: 2px; border-radius: 5px;
    padding: 0 6px; font: 600 12px/18px system-ui, sans-serif; }
  #prompthints .or { color: var(--dim); font-size: 11px; }
  body:not(.running) #prompthints, body.touch #prompthints { display: none; }
  `;
  let settingsOpen = false;
  let capture = null; // { id, button } while waiting for the input to bind
  let suppressClick = false;

  function buildPanel() {
    const style = document.createElement('style');
    style.textContent = css;
    document.head.appendChild(style);
    const btn = document.createElement('button');
    btn.id = 'setbtn'; btn.type = 'button'; btn.title = 'Settings (F1)'; btn.textContent = '⚙';
    btn.addEventListener('click', () => toggleSettings(true));
    document.getElementById('view').appendChild(btn);
    const root = document.createElement('div');
    root.id = 'settings';
    root.innerHTML = `
      <div class="panel" role="dialog" aria-label="Settings">
        <div class="head"><h2>Settings</h2><button class="x" type="button" title="Close (F1)" data-act="close">×</button></div>
        <div class="tabs" role="tablist" aria-label="Settings sections">
          <button class="tab" type="button" role="tab" data-tab="controls">Controls</button>
          <button class="tab" type="button" role="tab" data-tab="mouse">Mouse</button>
          <button class="tab" type="button" role="tab" data-tab="display">Display</button>
          <button class="tab" type="button" role="tab" data-tab="saves">Saved games</button>
          <button class="tab" type="button" role="tab" data-tab="storage">Storage</button>
        </div>
        <div class="pages">
          <section class="page" role="tabpanel" data-page="controls">
            <p class="note" style="margin-top:12px">Bindings act as an Xbox 360 controller. Click + then press a key, a mouse button or turn the wheel. Saved in this browser.</p>
            <div id="set-controls"></div>
            <div class="moved" id="set-moved"></div>
            <div class="foot"><button class="btn" type="button" data-act="reset">Reset controls to defaults</button></div>
          </section>
          <section class="page" role="tabpanel" data-page="mouse">
            <h3>Mouse look</h3>
            <div class="field"><label for="set-sens">Look sensitivity</label>
              <input type="range" id="set-sens" min="0.2" max="4" step="0.05"><span class="val" id="set-sens-val"></span></div>
            <div class="field"><label for="set-invert">Invert vertical look</label><input type="checkbox" id="set-invert"></div>
            <p class="note">Click the game to capture the mouse; Esc gives it back. Mouse buttons and the wheel are bound under Controls.</p>
          </section>
          <section class="page" role="tabpanel" data-page="display">
            <h3>Frame rate</h3>
            <div class="field"><label for="set-fps">Frame rate</label>
              <select id="set-fps"><option value="0" id="set-fps-match">Match the display</option><option value="60">Up to 60 fps</option><option value="120">Up to 120 fps</option><option value="30">30 fps, like the console</option></select></div>
            <p class="note" id="set-fps-note">The console version runs at 30 fps. Computers default to the display's refresh rate (60 to 144); phones, tablets and computers with 4 CPU threads or fewer default to 30: steadier, and it saves battery and heat. Changes apply the next time the game starts (reload the page).</p>
            <div class="field"><label for="set-quality">Graphics</label>
              <select id="set-quality"><option value="full">Full, like the console</option><option value="light">Lighter, for phones (fewer shadow passes)</option></select></div>
            <p class="note" id="set-quality-note">Lighter draws about 14% fewer objects per frame, which phones are limited by. It's the default on phones, tablets and computers with 4 CPU threads or fewer. Changes apply after a reload.</p>
            <div class="field"><label for="set-res">Resolution</label>
              <select id="set-res"><option value="1">Full, like the console (1152×720)</option><option value="0.75">75%: lighter on the graphics chip</option><option value="0.5">50%: fastest, blurrier</option></select></div>
            <p class="note" id="set-res-note">Renders the game's main picture smaller and scales it up, for integrated graphics and phones. The default is 75% on phones, tablets, small computers and Intel graphics. Changes apply after a reload.</p>
            <div class="field"><label for="set-showfps">Show frame rate</label><input type="checkbox" id="set-showfps"></div>
            <div class="field"><label for="set-promptkeys">Show keys for button prompts</label><input type="checkbox" id="set-promptkeys"></div>
            <p class="note">When the game shows a controller button ("A Select", "Hold Y Look"), the keys bound to it appear at the top of the screen.</p>
            <h3>Screen</h3>
            <div class="field"><label for="set-autofs">Fullscreen when playing</label><input type="checkbox" id="set-autofs"></div>
            <p class="note">Clicking or tapping the game to play switches to fullscreen. Esc leaves it; the ⛶ button and double-clicking the game toggle it any time.</p>
            <div class="field"><label for="set-awake">Keep the screen on</label><input type="checkbox" id="set-awake"></div>
            <p class="note" id="set-awake-note">The display doesn't dim or go to sleep while the game is open.</p>
          </section>
          <section class="page" role="tabpanel" data-page="saves">
            <h3>Checkpoint</h3>
            <div class="field"><label>Current checkpoint</label><span id="set-checkpoint" class="note" style="margin:0;flex:1">…</span>
              <button class="btn" type="button" data-act="delcheckpoint" id="set-delcheckpoint">Delete checkpoint</button></div>
            <p class="note">The save Resume Game continues from. Deleting it keeps your profile, settings and campaign progress; open Campaign and choose Replay Mission to play a mission again. The page reloads.</p>
            <h3>Everything</h3>
            <div class="field"><label>All saved data</label><span id="set-saves" class="note" style="margin:0;flex:1">…</span>
              <button class="btn" type="button" data-act="delsaves">Delete all</button></div>
            <p class="note">Checkpoints and your game profile, kept in this browser so they come back when you reload. Deleting them starts the game fresh. The page reloads.</p>
          </section>
          <section class="page" role="tabpanel" data-page="storage">
            <h3>Game data in this browser</h3>
            <div class="field"><label>Stored for faster loading</label><span id="set-storage" class="note" style="margin:0;flex:1">…</span>
              <button class="btn" type="button" data-act="clear">Clear</button></div>
            <p class="note">The game code and the disc data the game has read are kept here, so the next visit loads them locally. Clearing this doesn't touch saved games.</p>
            <h3>Diagnostics</h3>
            <div class="field"><label for="set-diag">Log performance details</label><input type="checkbox" id="set-diag"></div>
            <div class="field"><label>Report</label><span class="note" style="margin:0;flex:1">The game's log, with this device's details</span>
              <button class="btn" type="button" data-act="report">Save report</button></div>
            <p class="note" id="set-diag-note">If the game runs slowly on a device: turn on performance details, reload the page, play for a minute, then save the report and send it. The details cost a little speed, so turn them off afterwards.</p>
          </section>
        </div>
        <div class="foot"><span class="note" style="margin:0">F1 opens and closes settings.</span>
          <button class="btn primary" type="button" data-act="close">Done</button></div>
      </div>`;
    document.body.appendChild(root);
    // Tabs: click, or the arrow keys on a focused tab. The last tab is remembered.
    const tabs = [...root.querySelectorAll('.tab')];
    for (const t of tabs) {
      t.id = 'set-tab-' + t.dataset.tab;
      root.querySelector(`[data-page="${t.dataset.tab}"]`).setAttribute('aria-labelledby', t.id);
      t.addEventListener('click', () => showTab(t.dataset.tab));
      t.addEventListener('keydown', (e) => {
        const step = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
        if (!step) return;
        e.preventDefault();
        const next = tabs[(tabs.indexOf(t) + step + tabs.length) % tabs.length];
        showTab(next.dataset.tab); next.focus();
      });
    }
    showTab(load('crysisSettingsTab', 'controls'));
    root.addEventListener('click', (e) => {
      const act = e.target.dataset && e.target.dataset.act;
      if (e.target === root || act === 'close') toggleSettings(false);
      else if (act === 'reset') { bindings = clone(DEFAULTS); rebuildLookup(); saveBindings(); renderControls(); setMoved(''); }
      else if (act === 'clear') clearStorage();
      else if (act === 'delsaves') deleteSaves(false);
      else if (act === 'delcheckpoint') deleteSaves(true);
      else if (act === 'report' && window.crysis && crysis.report) crysis.report();
    });
    const sens = document.getElementById('set-sens'), inv = document.getElementById('set-invert');
    sens.addEventListener('input', () => { mouse.sensitivity = Number(sens.value); document.getElementById('set-sens-val').textContent = mouse.sensitivity.toFixed(2); save('crysisMouse', mouse); applyMouse(); });
    inv.addEventListener('change', () => { mouse.invertY = inv.checked; save('crysisMouse', mouse); applyMouse(); });
    // Read by the page before the game starts (index.html preRun).
    const fps = document.getElementById('set-fps');
    fps.addEventListener('change', () => {
      save('crysisVideo', Object.assign(load('crysisVideo', {}) || {}, { fps: Number(fps.value) }));
      document.getElementById('set-fps-note').textContent = 'Saved. Reload the page for the new frame rate to take effect.';
    });
    // The frame-rate meter, fullscreen and screen settings (index.html) apply at once.
    const setVideo = (change) => save('crysisVideo', Object.assign(load('crysisVideo', {}) || {}, change));
    const showFps = document.getElementById('set-showfps');
    showFps.addEventListener('change', () => {
      setVideo({ showFps: showFps.checked });
      if (window.crysisFpsMeter) crysisFpsMeter.show(showFps.checked);
    });
    // Read by the page before the game starts (index.html preRun): PROFILE=1.
    const diag = document.getElementById('set-diag');
    diag.addEventListener('change', () => {
      try { localStorage.setItem('crysisDiag', diag.checked ? '1' : '0'); } catch (e) {}
      document.getElementById('set-diag-note').textContent = 'Saved. Reload the page for this to take effect.';
    });
    const promptKeys = document.getElementById('set-promptkeys');
    promptKeys.addEventListener('change', () => setVideo({ promptKeys: promptKeys.checked }));
    // Read by the page before the game starts (index.html preRun).
    const quality = document.getElementById('set-quality');
    quality.addEventListener('change', () => {
      setVideo({ quality: quality.value });
      document.getElementById('set-quality-note').textContent = 'Saved. Reload the page for the new graphics setting to take effect.';
    });
    const res = document.getElementById('set-res');
    res.addEventListener('change', () => {
      setVideo({ res: Number(res.value) });
      document.getElementById('set-res-note').textContent = 'Saved. Reload the page for the new resolution to take effect.';
    });
    const autoFs = document.getElementById('set-autofs'), awake = document.getElementById('set-awake');
    autoFs.addEventListener('change', () => setVideo({ autoFullscreen: autoFs.checked }));
    awake.addEventListener('change', () => {
      setVideo({ keepAwake: awake.checked });
      if (window.crysisDisplay) crysisDisplay.updateWakeLock();
    });
    // Capturing a new binding (capture phase: before the game's handlers).
    window.addEventListener('keydown', (e) => { if (capture) { e.preventDefault(); e.stopPropagation(); bind('Key:' + e.code); } }, true);
    root.addEventListener('mousedown', (e) => {
      if (!capture || e.target === capture.button) return; // clicking + again cancels
      e.preventDefault(); suppressClick = true; bind('Mouse:' + e.button);
    }, true);
    // (The click that follows a bound mouse button must not press what it landed on.)
    root.addEventListener('click', (e) => { if (suppressClick) { suppressClick = false; e.stopPropagation(); e.preventDefault(); } }, true);
    root.addEventListener('wheel', (e) => { if (capture) { e.preventDefault(); bind(e.deltaY < 0 ? 'Wheel:Up' : 'Wheel:Down'); } }, { passive: false });
    root.addEventListener('contextmenu', (e) => { if (capture) e.preventDefault(); });
  }

  function showTab(name) {
    const root = document.getElementById('settings');
    if (!root.querySelector(`.page[data-page="${name}"]`)) name = 'controls';
    for (const t of root.querySelectorAll('.tab')) {
      const on = t.dataset.tab === name;
      t.setAttribute('aria-selected', String(on));
      t.tabIndex = on ? 0 : -1;
    }
    for (const p of root.querySelectorAll('.page')) p.hidden = p.dataset.page !== name;
    root.querySelector('.pages').scrollTop = 0;
    save('crysisSettingsTab', name);
  }

  function setMoved(text) { document.getElementById('set-moved').textContent = text; }
  const controlLabel = (id) => { const c = CONTROLS.find((x) => x.id === id); return c ? c.pad + (c.hint ? ' (' + c.hint + ')' : '') : String(id); };

  function bind(input) {
    const { id } = capture;
    capture = null;
    // One input drives one control: take it away from any other.
    const from = [];
    for (const [other, inputs] of Object.entries(bindings)) {
      const i = inputs.indexOf(input);
      if (i >= 0 && Number(other) !== id) { inputs.splice(i, 1); from.push(Number(other)); }
    }
    (bindings[id] ||= []);
    if (!bindings[id].includes(input)) bindings[id].push(input);
    rebuildLookup(); saveBindings(); renderControls();
    setMoved(from.length ? `${inputName(input)} was moved from ${from.map(controlLabel).join(', ')}.` : '');
  }

  function renderControls() {
    const host = document.getElementById('set-controls');
    host.textContent = '';
    for (const c of CONTROLS) {
      if (c.group) { const h = document.createElement('h3'); h.textContent = c.group; host.appendChild(h); continue; }
      const row = document.createElement('div'); row.className = 'row';
      const what = document.createElement('div'); what.className = 'what';
      what.innerHTML = '<b></b><span></span>';
      what.querySelector('b').textContent = c.pad;
      what.querySelector('span').textContent = c.hint || '';
      const chips = document.createElement('div'); chips.className = 'chips';
      for (const input of bindings[c.id] || []) {
        const chip = document.createElement('span'); chip.className = 'chip';
        chip.append(inputName(input));
        const x = document.createElement('button'); x.type = 'button'; x.title = 'Remove'; x.textContent = '×';
        x.addEventListener('click', () => {
          bindings[c.id] = (bindings[c.id] || []).filter((v) => v !== input);
          rebuildLookup(); saveBindings(); renderControls(); setMoved('');
        });
        chip.appendChild(x); chips.appendChild(chip);
      }
      const add = document.createElement('button'); add.type = 'button'; add.className = 'add'; add.textContent = '+';
      add.title = 'Add a key, mouse button or wheel direction';
      add.addEventListener('click', () => {
        if (capture && capture.button === add) { capture = null; add.classList.remove('listening'); add.textContent = '+'; return; }
        if (capture) { capture.button.classList.remove('listening'); capture.button.textContent = '+'; }
        capture = { id: c.id, button: add };
        add.classList.add('listening'); add.textContent = 'Press a key, mouse button or wheel… (click to cancel)';
      });
      chips.appendChild(add);
      row.append(what, chips);
      host.appendChild(row);
    }
  }

  async function showStorage() {
    const el = document.getElementById('set-storage');
    try {
      const { usage, quota } = await navigator.storage.estimate();
      const gb = (b) => (b / 1073741824).toFixed(b < 1073741824 ? 2 : 1) + ' GB';
      el.textContent = `${gb(usage || 0)} used of ${gb(quota || 0)} available to this site`;
    } catch (e) { el.textContent = 'not available in this browser'; }
    showSaves();
  }
  const when = (t) => new Date(t).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
  function showSaves() {
    const el = document.getElementById('set-saves'), cp = document.getElementById('set-checkpoint');
    const delCp = document.getElementById('set-delcheckpoint');
    const s = window.crysisSaves && crysisSaves.info();
    delCp.disabled = true;
    if (!s) { el.textContent = cp.textContent = 'not available'; return; }
    if (s.error) { el.textContent = cp.textContent = 'not kept: ' + s.error; return; }
    el.textContent = s.files
      ? `${s.files} file${s.files === 1 ? '' : 's'}, ${(s.bytes / 1024).toFixed(0)} KB` + (s.lastSaved ? ` · last saved ${when(s.lastSaved)}` : '')
      : 'none yet';
    cp.textContent = '…';
    crysisSaves.checkpoints().then(({ current }) => {
      cp.textContent = current ? 'saved ' + (current.saved ? when(current.saved) : 'earlier') : 'none';
      delCp.disabled = !current;
    }).catch(() => { cp.textContent = 'not available'; });
  }
  async function deleteSaves(onlyCheckpoint) {
    if (!window.crysisSaves) return;
    const question = onlyCheckpoint
      ? 'Delete the current checkpoint?\n\nResume Game will no longer continue from it. Your profile, settings and campaign progress stay. This can\'t be undone, and the page reloads.'
      : 'Delete all saved data: checkpoints and your game profile (settings and campaign progress)?\n\nThe game starts fresh. This can\'t be undone, and the page reloads.';
    if (!confirm(question)) return;
    const el = document.getElementById(onlyCheckpoint ? 'set-checkpoint' : 'set-saves');
    el.textContent = 'deleting…';
    try { await (onlyCheckpoint ? crysisSaves.deleteCheckpoint() : crysisSaves.deleteAll()); } // reloads the page
    catch (e) { el.textContent = 'could not delete: ' + (e && e.message || e); }
  }
  async function clearStorage() {
    const el = document.getElementById('set-storage');
    el.textContent = 'clearing…';
    try {
      for (const name of await caches.keys()) if (name.startsWith('crysis-')) await caches.delete(name);
      if (navigator.serviceWorker && navigator.serviceWorker.controller) navigator.serviceWorker.controller.postMessage('forget');
    } catch (e) {}
    showStorage();
  }

  function toggleSettings(open = !settingsOpen) {
    settingsOpen = open;
    blocked = open;
    if (open) {
      releaseAll();
      if (document.pointerLockElement) document.exitPointerLock();
      document.getElementById('set-sens').value = mouse.sensitivity;
      document.getElementById('set-sens-val').textContent = Number(mouse.sensitivity).toFixed(2);
      document.getElementById('set-invert').checked = !!mouse.invertY;
      const video = load('crysisVideo', {}) || {};
      const weak = window.crysisWeakDevice ?? isTouch;
      const fpsChoice = video.fps ?? (weak ? 30 : 0);
      document.getElementById('set-fps').value = [0, 30, 60, 120].includes(fpsChoice) ? String(fpsChoice) : '60';
      const hz = window.crysisRefreshHz && crysisRefreshHz();
      document.getElementById('set-fps-match').textContent = hz ? `Match the display (${Math.min(144, Math.max(60, hz))} fps)` : 'Match the display';
      document.getElementById('set-showfps').checked = video.showFps !== false;
      document.getElementById('set-autofs').checked = video.autoFullscreen !== false;
      document.getElementById('set-promptkeys').checked = video.promptKeys !== false;
      document.getElementById('set-quality').value = video.quality || (weak ? 'light' : 'full');
      const resChoice = video.res ?? (window.crysisDefaultRes ? crysisDefaultRes() : 1);
      document.getElementById('set-res').value = [1, 0.75, 0.5].includes(resChoice) ? String(resChoice) : '1';
      try { document.getElementById('set-diag').checked = localStorage.getItem('crysisDiag') === '1'; } catch (e) {}
      const awake = document.getElementById('set-awake'), canKeepOn = !!(window.crysisDisplay && crysisDisplay.wakeLockSupported);
      awake.checked = canKeepOn && video.keepAwake !== false;
      awake.disabled = !canKeepOn;
      document.getElementById('set-awake-note').textContent = canKeepOn
        ? 'The display doesn\'t dim or go to sleep while the game is open.'
        : 'This browser can\'t keep the screen on (it needs the Screen Wake Lock API and a secure page: localhost or https).';
      renderControls(); setMoved(''); showStorage();
    } else capture = null;
    document.getElementById('settings').classList.toggle('open', open);
  }

  // ---- Keys for the game's button prompts --------------------------------------
  // The runtime reports which controller buttons' icons the game shows right now
  // ("A Select", "Hold Y Look"): Module._crysis_prompt_buttons(), a mask of pad
  // control ids (runtime/prompts.cpp). The keys bound to them appear at the top
  // of the screen. Settings, Display: Show keys for button prompts.
  const PROMPT_GROUPS = [ // pad label, icon style, control ids
    ['A', 'a', [0]], ['B', 'b', [1]], ['X', 'x', [2]], ['Y', 'y', [3]],
    ['LB', '', [4]], ['RB', '', [5]], ['LT', '', [14]], ['RT', '', [15]],
    ['LS', '', [8]], ['RS', '', [9]], ['BACK', '', [6]], ['START', '', [7]],
    ['D-pad', '', [10, 11, 12, 13]], ['Move', '', [16, 17, 18, 19]], ['Look', '', [20, 21, 22, 23]],
  ];
  let promptMask = -1, promptVersion = -1;
  const promptsOn = () => (load('crysisVideo', {}) || {}).promptKeys !== false;
  function span(cls, text) { const s = document.createElement('span'); s.className = cls; if (text) s.textContent = text; return s; }
  function renderPrompts(mask) {
    const host = document.getElementById('prompthints');
    host.textContent = '';
    for (const [label, cls, ids] of PROMPT_GROUPS) {
      const on = ids.filter((id) => mask & (1 << id));
      if (!on.length) continue;
      let names;
      if (label === 'Look') names = ['Mouse'];
      else if (ids.length > 1) names = on.map((id) => (bindings[id] || [])[0]).filter(Boolean).map(inputName); // one key per direction
      else names = [...new Set((bindings[ids[0]] || []).map(inputName))].slice(0, 2);
      const hint = span('hint');
      hint.append(span('pad ' + cls, label));
      if (!names.length) names = ['not bound'];
      names.forEach((n, i) => {
        if (i && ids.length === 1) hint.append(span('or', 'or'));
        hint.append(span('key', n));
      });
      host.append(hint);
    }
    host.classList.toggle('shown', host.childElementCount > 0);
  }
  function pollPrompts() {
    let mask = 0;
    if (!settingsOpen && !isTouch && document.visibilityState === 'visible' && document.body.classList.contains('running') && promptsOn())
      try { mask = window.Module && Module._crysis_prompt_buttons ? Module._crysis_prompt_buttons() >>> 0 : 0; } catch (e) {}
    if (mask === promptMask && bindingsVersion === promptVersion) return;
    promptMask = mask; promptVersion = bindingsVersion;
    renderPrompts(mask);
  }

  function init() {
    buildPanel();
    setupMouse();
    const hints = document.createElement('div');
    hints.id = 'prompthints';
    hints.setAttribute('aria-live', 'polite');
    document.getElementById('view').appendChild(hints);
    setInterval(pollPrompts, 150);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();

  window.crysisControls = {
    onRuntime: applyMouse, // the game's runtime is up: send the mouse settings
    open: () => toggleSettings(true),
  };
  // Console helpers: crysis.mouse({ sensitivity: 1.5, invertY: true }), crysis.settings().
  window.crysis = window.crysis || {};
  window.crysis.mouse = (opts = {}) => { Object.assign(mouse, opts); save('crysisMouse', mouse); applyMouse(); return { ...mouse }; };
  window.crysis.settings = () => toggleSettings(true);
})();
