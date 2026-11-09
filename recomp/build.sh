#!/bin/sh
# Build the recompiled code and the runtime into build/crysis2.exe.
# Recompiled files are compiled once and reused until their source changes.
# Paths are relative to recomp/ so the parent folder's name (which contains
# parentheses) never appears in a shell command.
set -e
cd "$(dirname "$0")"
export PATH="$(pwd)/../tools/toolchain/llvm-mingw/bin:$PATH"
XR=../tools/XenonRecomp
JOBS="${JOBS:-12}"

export CXXFLAGS="-std=c++20 -O1 -gline-tables-only -march=x86-64-v3 -DPPC_TIMEBASE_EXTERNAL -DPPC_FOLD_PHYSICAL_VIEWS -Wno-everything -Ippc -I$XR/thirdparty/simde"

mkdir -p build/ppc build/runtime

echo "== recompiled code"
ls ppc/*.cpp | xargs -P "$JOBS" -I{} sh -c '
    src="{}"; obj="build/ppc/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        clang++ $CXXFLAGS -c "$src" -o "$obj" || { echo "FAILED $src"; exit 255; }
    fi'

echo "== runtime"
python -I runtime/gen_stubs.py ppc runtime
WGPU=../tools/wgpu
RTFLAGS="$CXXFLAGS -g -Iruntime -I$WGPU/include -I$XR/XenonUtils -I$XR/thirdparty/disasm -DNOMINMAX -include cstdlib -include algorithm -include thread -include vector -include memory"
NEWEST_H=$(ls -t runtime/*.h | head -1) # any header change rebuilds the runtime (no dependency tracking)
for src in runtime/*.cpp; do
    obj="build/runtime/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$NEWEST_H" -nt "$obj" ]; then
        echo "   $src"
        clang++ $RTFLAGS -Wall -Wno-unused-function -c "$src" -o "$obj"
    fi
done

echo "== link"
B=$XR/build
clang++ -g -o build/crysis2.exe build/runtime/*.o build/ppc/*.o \
    $B/XenonUtils/libXenonUtils.a $B/thirdparty/disasm/libdisasm.a \
    $WGPU/lib/libwgpu_native.dll.a -lgdi32 -luser32 \
    -static -Wl,--stack,67108864
# The WebGPU implementation is a DLL; keep it next to the exe and in run/.
mkdir -p run
cp -u $WGPU/lib/wgpu_native.dll build/
cp -u $WGPU/lib/wgpu_native.dll run/
# Overlay defaults (system.cfg with the intro movies skipped) go into run/overlay once.
mkdir -p run/overlay
[ -f run/overlay/system.cfg ] || cp overlay-template/system.cfg run/overlay/
echo "built build/crysis2.exe"
