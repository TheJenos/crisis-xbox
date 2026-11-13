# crisis-xbox

A static recompilation of the Xbox 360 version of Crysis 2 that runs in a web
browser. The game's PowerPC code is translated to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), then compiled to
WebAssembly with Emscripten. A runtime in this repository does the work of the
console: kernel and XAM imports, threads, file system, audio (XMA) and the
Xenos GPU on top of WebGPU.

**This repository contains no game data.** It has no disc image, no files from
the disc, no code generated from the game, and no builds. You need your own
copy of the game (title 454108E3) to build and play it. This project is not
affiliated with Crytek or Microsoft.

## Layout

| Path | Contents |
|---|---|
| `recomp/runtime/` | Host runtime: kernel, threads, file system, input, audio, Xenos GPU emulation (command processor, shader and texture translation, WebGPU backend) |
| `recomp/web-src/` | Browser page: loader, settings and key bindings (`controls.js`), saved games in IndexedDB (`saves.js`), disc cache service worker (`sw.js`), multiplayer links (`mp.js`) |
| `recomp/firebase/` | Multiplayer: Realtime Database rules |
| `recomp/config/` | XenonRecomp configuration (`Crysis2.toml`) and switch tables |
| `recomp/overlay-template/system.cfg` | Game settings embedded into the build |
| `recomp/build_web.sh` | Browser build |
| `recomp/serve.py` | Local server for the browser build (127.0.0.1 only) |
| `recomp/build.sh` | Native Windows build (reference only) |
| `analysis/` | Tools used along the way (XEX and disc readers, shader tools, instruction tests) and the XenonRecomp patch |

## Building

You need Python 3, Git Bash or another POSIX shell, and a browser with WebGPU
(recent Chrome or Edge).

1. **Disc.** Put your disc image in the repository root. Extract the
   executable and the disc files:

   ```sh
   python -I analysis/xiso.py "<image>.iso" extract default.xex extracted/default.xex
   python -I analysis/xiso.py "<image>.iso" extractall extracted/disc
   ```

2. **XenonRecomp.** Clone it into `tools/XenonRecomp` at commit `ddd128b`,
   apply `analysis/XenonRecomp-crysis2.patch`, and build it:

   ```sh
   git clone --recursive https://github.com/hedge-dev/XenonRecomp tools/XenonRecomp
   git -C tools/XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
   git -C tools/XenonRecomp apply ../../analysis/XenonRecomp-crysis2.patch
   ```

3. **Recompile.** Run XenonRecomp on `recomp/config/Crysis2.toml` with
   `tools/XenonRecomp/XenonUtils/ppc_context.h`. It writes the C++ to
   `recomp/ppc/`.

4. **Emscripten.** Install emsdk 7.0.0 into `tools/emsdk`.

5. **Build.** Run `sh recomp/build_web.sh`. The first build compiles all the
   recompiled code. It takes a while and needs a lot of memory, so
   `JOBS=6 sh recomp/build_web.sh` is safer. The output goes to `recomp/web/`.

6. **Play.** Run `python -I recomp/serve.py extracted/disc 8080` and open
   http://127.0.0.1:8080/. Press F1 for settings and key bindings.

## Multiplayer (experimental)

Crysis 2's multiplayer on the Xbox 360 is Xbox LIVE only. The runtime stands in
for LIVE: it signs the player in with a gamertag, lists the sessions other
players host, and carries the game's network packets between browsers.

- `recomp/runtime/mp.cpp`: XGI sessions (create, join, search, start, end), QoS
  and the player's identity: a 24-bit player ID kept per browser tab, with its
  own 10.x.y.z address and XUIDs.
- `recomp/web-src/mp.js`: the links between pages. Sessions and players
  (gamertag, status: menus, lobby, in match) are listed in a Firebase Realtime
  Database, through its REST API and EventSource (no SDK). The game's packets go
  player to player over WebRTC data channels; the database only carries the
  connection offers. With `?mp=local` the link is a `BroadcastChannel` between
  tabs of one browser, for testing.
- `recomp/firebase/database.rules.json`: who may write what. Only
  `c2/sessions`, `c2/players` and `c2/signal` exist; every field is checked.

To set it up:

1. Create a Firebase project with a Realtime Database. From `recomp/firebase/`,
   run `firebase use <project>` and `firebase deploy --only database`.
2. Add a web app to the project and save its config as
   `recomp/firebase-config.json` (`firebase apps:sdkconfig web`; only
   `databaseURL` is used). The build copies it next to the page. It is not in
   this repository.
3. In the game: Settings (F1) > Multiplayer > Play online, pick a gamertag and
   reload. `?mp=online` or `?mp=local` in the URL does the same for one visit.

Anyone who opens the page can read the session and player lists, and WebRTC
connection offers include the player's network addresses.

What works so far: the multiplayer menus, Quick Match finding a session hosted
in another browser and joining it, with both players listed in its lobby
(internet link), and a private match loading its map for the host alone. Not
done yet: Quick Match waits for its minimum number of players, so two players
alone don't reach a match; the host doesn't answer a join over the `?mp=local`
link; no TURN server (some networks block direct connections); no host
migration or leaderboards.

## Debugging

Add `?env=NAME=value` to the page URL to set runtime options. For example,
`PROFILE=1` logs per-thread CPU use, command processor phases and GPU timing
every 10 seconds.
