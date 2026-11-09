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
| `recomp/web-src/` | Browser page: loader, settings and key bindings (`controls.js`), saved games in IndexedDB (`saves.js`), disc cache service worker (`sw.js`) |
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

## Debugging

Add `?env=NAME=value` to the page URL to set runtime options. For example,
`PROFILE=1` logs per-thread CPU use, command processor phases and GPU timing
every 10 seconds.
