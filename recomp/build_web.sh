#!/bin/sh
# Build the browser version into web/: crysis2.js + crysis2.wasm (+ index.html).
# Recompiled files are compiled once and reused until their source changes.
# Paths are relative to recomp/ so the parent folder's name (which contains
# parentheses) never appears in a shell command.
set -e
cd "$(dirname "$0")"
EM=../tools/emsdk/upstream/emscripten
export EMXX="$EM/em++.exe"
export EMCC="$EM/emcc.exe"
XR=../tools/XenonRecomp
JOBS="${JOBS:-12}"

export WEBFLAGS="-std=c++20 -O2 -g0 -msimd128 -pthread -fwasm-exceptions -DPPC_TIMEBASE_EXTERNAL -DPPC_FOLD_PHYSICAL_VIEWS -Wno-everything -Ippc -I$XR/thirdparty/simde"

mkdir -p build-web/ppc-O2 build-web/runtime build-web/xenonutils web

echo "== recompiled code"
ls ppc/*.cpp | xargs -P "$JOBS" -I{} sh -c '
    src="{}"; obj="build-web/ppc-O2/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        "$EMXX" $WEBFLAGS -c "$src" -o "$obj" || { echo "FAILED $src"; exit 255; }
    fi'

echo "== runtime"
python -I runtime/gen_stubs.py ppc runtime
RTFLAGS="$WEBFLAGS -O2 --use-port=emdawnwebgpu -Iruntime -I$XR/XenonUtils -I$XR/thirdparty/disasm -include cstdlib -include algorithm -include thread -include vector -include memory"
NEWEST_H=$(ls -t runtime/*.h | head -1) # any header change rebuilds the runtime (no dependency tracking)
export RTFLAGS NEWEST_H
ls runtime/*.cpp | xargs -P "${RT_JOBS:-6}" -I{} sh -c '
    src="{}"; obj="build-web/runtime/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$NEWEST_H" -nt "$obj" ]; then
        echo "   $src"
        "$EMXX" $RTFLAGS -Wall -Wno-unused-function -c "$src" -o "$obj" || { echo "FAILED $src"; exit 255; }
    fi'

echo "== XEX loader (XenonUtils subset)"
XU=$XR/XenonUtils
XUFLAGS="-O1 -g0 -pthread -fwasm-exceptions -Wno-everything -I$XU -I$XR/thirdparty/simde -I$XR/thirdparty/libmspack/libmspack/mspack -I$XR/thirdparty/tiny-AES-c -I$XR/thirdparty/TinySHA1 -I$XR/thirdparty/disasm -I$XR/thirdparty/fmt/include -DNOMINMAX -DFMT_HEADER_ONLY"
[ build-web/xenonutils/xex.o -nt $XU/xex.cpp ] || "$EMXX" -std=c++20 $XUFLAGS -c $XU/xex.cpp -o build-web/xenonutils/xex.o
[ build-web/xenonutils/image.o -nt $XU/image.cpp ] || "$EMXX" -std=c++20 $XUFLAGS -c $XU/image.cpp -o build-web/xenonutils/image.o
[ build-web/xenonutils/xex_patcher.o -nt $XU/xex_patcher.cpp ] || "$EMXX" -std=c++20 $XUFLAGS -c $XU/xex_patcher.cpp -o build-web/xenonutils/xex_patcher.o
[ -f build-web/xenonutils/lzxd.o ] || "$EMCC" $XUFLAGS -c $XR/thirdparty/libmspack/libmspack/mspack/lzxd.c -o build-web/xenonutils/lzxd.o
[ -f build-web/xenonutils/aes.o ] || "$EMCC" $XUFLAGS -c $XR/thirdparty/tiny-AES-c/aes.c -o build-web/xenonutils/aes.o

echo "== link"
# Function names stay in the wasm (browser CPU profiles and crash stacks show
# sub_XXXXXXXX and runtime functions instead of numbers): 2 MB larger, same
# speed. NAMES=0 leaves them out.
NAMEFLAGS="--profiling-funcs"
[ "$NAMES" = "0" ] && NAMEFLAGS=""
"$EMXX" -O1 -g0 $NAMEFLAGS -pthread -fwasm-exceptions -msimd128 \
    build-web/runtime/*.o build-web/ppc-O2/*.o build-web/xenonutils/*.o \
    --use-port=emdawnwebgpu \
    -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=64 \
    -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=4GB \
    -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB \
    -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0 \
    -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='$UTF8ToString' \
    -sEXPORTED_FUNCTIONS=_main,_crysis_touch_buttons,_crysis_touch_axis,_crysis_disc_bytes,_crysis_disc_busy,_crysis_dump_threads,_crysis_peek32,_crysis_dump_frame,_crysis_shader_wgsl,_crysis_log_consts,_crysis_skip_ps,_crysis_mouse_move,_crysis_mouse_config,_crysis_watch_const,_crysis_watch_value,_crysis_control,_crysis_saves_writing,_crysis_saves_changes,_crysis_debug_save,_crysis_checkpoint_failures,_crysis_prompt_buttons \
    --embed-file overlay-template/system.cfg@/overlay/system.cfg \
    --pre-js web-src/pre.js \
    -o web/crysis2.js
# Page, key bindings / settings, saved games, and the service worker that caches disc reads.
cp web-src/index.html web-src/controls.js web-src/saves.js web-src/sw.js web/
echo "built web/crysis2.js + web/crysis2.wasm ($(du -h web/crysis2.wasm | cut -f1))"
