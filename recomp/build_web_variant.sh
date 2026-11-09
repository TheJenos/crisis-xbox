#!/bin/sh
# Build a variant of the browser version from another recompiled-code folder,
# without touching build_web.sh's outputs (ppc/, build-web/ppc-O2, build-web/runtime, web/).
# Used to try XenonRecomp options (e.g. *_as_local registers) side by side.
#   PPC=ppc-loc OUT=web-loc JOBS=6 sh build_web_variant.sh
# Objects go to build-web/<PPC>-O2 and build-web/runtime-<PPC> (the runtime is
# compiled against the variant's ppc_config.h, whose register options change
# the PPCContext layout). Paths are relative to recomp/.
set -e
cd "$(dirname "$0")"
PPC="${PPC:?set PPC to the recompiled-code folder, e.g. ppc-loc}"
OUT="${OUT:?set OUT to the output folder, e.g. web-loc}"
EM=../tools/emsdk/upstream/emscripten
export EMXX="$EM/em++.exe"
export EMCC="$EM/emcc.exe"
XR=../tools/XenonRecomp
JOBS="${JOBS:-6}"
PPCOBJ="build-web/$PPC-O2"
RTOBJ="build-web/runtime-$PPC"
export PPCOBJ RTOBJ

export WEBFLAGS="-std=c++20 -O2 -g0 -msimd128 -pthread -fwasm-exceptions -DPPC_TIMEBASE_EXTERNAL -DPPC_FOLD_PHYSICAL_VIEWS -Wno-everything -I$PPC -I$XR/thirdparty/simde"

mkdir -p "$PPCOBJ" "$RTOBJ" build-web/xenonutils "$OUT"

echo "== recompiled code ($PPC)"
ls $PPC/*.cpp | xargs -P "$JOBS" -I{} sh -c '
    src="{}"; obj="$PPCOBJ/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        "$EMXX" $WEBFLAGS -c "$src" -o "$obj" || { echo "FAILED $src"; exit 255; }
    fi'

echo "== runtime"
RTFLAGS="$WEBFLAGS -O2 --use-port=emdawnwebgpu -Iruntime -I$XR/XenonUtils -I$XR/thirdparty/disasm -include cstdlib -include algorithm -include thread -include vector -include memory"
NEWEST_H=$(ls -t runtime/*.h $PPC/ppc_config.h | head -1)
export RTFLAGS NEWEST_H
ls runtime/*.cpp | xargs -P "${RT_JOBS:-6}" -I{} sh -c '
    src="{}"; obj="$RTOBJ/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$NEWEST_H" -nt "$obj" ]; then
        echo "   $src"
        "$EMXX" $RTFLAGS -Wall -Wno-unused-function -c "$src" -o "$obj" || { echo "FAILED $src"; exit 255; }
    fi'

echo "== XEX loader: reusing build-web/xenonutils (build_web.sh builds it)"
ls build-web/xenonutils/*.o > /dev/null

echo "== link"
NAMEFLAGS="--profiling-funcs"
[ "$NAMES" = "0" ] && NAMEFLAGS=""
"$EMXX" -O1 -g0 $NAMEFLAGS -pthread -fwasm-exceptions -msimd128 \
    "$RTOBJ"/*.o "$PPCOBJ"/*.o build-web/xenonutils/*.o \
    --use-port=emdawnwebgpu \
    -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=64 \
    -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=4GB \
    -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB \
    -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0 \
    -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='$UTF8ToString' \
    -sEXPORTED_FUNCTIONS=_main,_crysis_touch_buttons,_crysis_touch_axis,_crysis_disc_bytes,_crysis_disc_busy,_crysis_dump_threads,_crysis_peek32,_crysis_dump_frame,_crysis_shader_wgsl,_crysis_log_consts,_crysis_skip_ps,_crysis_mouse_move,_crysis_mouse_config,_crysis_watch_const,_crysis_watch_value,_crysis_control \
    --embed-file overlay-template/system.cfg@/overlay/system.cfg \
    -o "$OUT/crysis2.js"
cp web-src/index.html web-src/controls.js web-src/saves.js web-src/sw.js "$OUT/"
echo "built $OUT/crysis2.js + $OUT/crysis2.wasm ($(du -h "$OUT/crysis2.wasm" | cut -f1))"
