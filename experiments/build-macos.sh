#!/bin/sh
# Build the pinned release from a user-supplied US ROM. No ROM is downloaded.
set -eu
cd "$(dirname "$0")/.."
recomp_root=$PWD
tools_root="${BM64_TOOLS_ROOT:-$recomp_root/../tools}"
rom_path="${1:-$recomp_root/../Bomberman 64 (USA).z64}"
jobs="${BUILD_JOBS:-8}"
llvm_root="${LLVM_ROOT:-/opt/homebrew/opt/llvm@18}"

test "$(uname -s)" = Darwin || { echo 'This recipe targets macOS.' >&2; exit 1; }
test -f "$rom_path" || { echo "ROM not found: $rom_path" >&2; exit 1; }
rom_hash=$(shasum -a 256 "$rom_path" | cut -d ' ' -f 1)
test "$rom_hash" = e6da7c26127788cd894b88b71cc055ff9dec0d0f4f8e10d9b15b40153af2b52a || {
    echo 'Expected the verified US 1.0 big-endian ROM; refusing an unverified input.' >&2
    exit 1
}
test "$(git -C "$tools_root/N64Recomp" rev-parse HEAD)" = 98bf104b1b5ed83126af8bcab0cc964782617dbf
test "$(git -C "$tools_root/bm64" rev-parse HEAD)" = c7c5fd5a73a9d5969673535030fc4f1df956fa5f
test "$(git -C "$tools_root/SDL2" rev-parse HEAD)" = fb1497566c5a05e2babdcf45ef0ab5c7cca2c4ae
test -f BM64Syms/dump.toml
test -x "$llvm_root/bin/ld.lld"

dependency_patch="$recomp_root/experiments/patches/hlslpp-stdlib.patch"
if ! git -C lib/rt64/src/contrib/hlslpp apply --reverse --check "$dependency_patch" 2>/dev/null; then
    git -C lib/rt64/src/contrib/hlslpp apply --check "$dependency_patch"
    git -C lib/rt64/src/contrib/hlslpp apply "$dependency_patch"
fi

runtime_patch="$recomp_root/experiments/patches/runtime-checkpoint.patch"
if ! git -C lib/N64ModernRuntime apply --reverse --check "$runtime_patch" 2>/dev/null; then
    git -C lib/N64ModernRuntime apply --check "$runtime_patch"
    git -C lib/N64ModernRuntime apply "$runtime_patch"
fi

renderer_patch="$recomp_root/experiments/patches/rt64-catchup.patch"
if ! git -C lib/rt64 apply --reverse --check "$renderer_patch" 2>/dev/null; then
    git -C lib/rt64 apply --check "$renderer_patch"
    git -C lib/rt64 apply "$renderer_patch"
fi

rsp_patch="$recomp_root/experiments/patches/rt64-rsp-rounding.patch"
if ! git -C lib/rt64 apply --reverse --check "$rsp_patch" 2>/dev/null; then
    git -C lib/rt64 apply --check "$rsp_patch"
    git -C lib/rt64 apply "$rsp_patch"
fi

descriptor_patch="$recomp_root/experiments/patches/plume-descriptor-layout.patch"
if ! git -C lib/rt64/src/contrib/plume apply --reverse --check "$descriptor_patch" 2>/dev/null; then
    git -C lib/rt64/src/contrib/plume apply --check "$descriptor_patch"
    git -C lib/rt64/src/contrib/plume apply "$descriptor_patch"
fi

presentation_patch="$recomp_root/experiments/patches/plume-presentation-probe.patch"
if ! git -C lib/rt64/src/contrib/plume apply --reverse --check "$presentation_patch" 2>/dev/null; then
    git -C lib/rt64/src/contrib/plume apply --check "$presentation_patch"
    git -C lib/rt64/src/contrib/plume apply "$presentation_patch"
fi

# Xcode 26.6 may need TOOLCHAINS set explicitly even after component installation.
xcrun -sdk macosx metal --version
cmake -S "$tools_root/SDL2" -B "$tools_root/SDL2/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$tools_root/sdl2-install" \
    -DSDL_TEST=OFF -DSDL_SHARED=ON -DSDL_STATIC=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$tools_root/SDL2/build" -j "$jobs"
cmake --install "$tools_root/SDL2/build"
cmake -S "$tools_root/N64Recomp" -B "$tools_root/N64Recomp/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$tools_root/N64Recomp/build" --target N64Recomp RSPRecomp -j "$jobs"
cp "$tools_root/N64Recomp/build/N64Recomp" "$tools_root/N64Recomp/build/RSPRecomp" .

clang++ -std=c++17 -O2 "$tools_root/bm64/tools/bm64decompress.cpp" -o "$tools_root/bm64/tools/bm64decompress"
"$tools_root/bm64/tools/bm64decompress" "$rom_path" bm64.decomp.us.z64
test "$(shasum -a 256 bm64.decomp.us.z64 | cut -d ' ' -f 1)" = db712de814fb56a958d21c7bbf19563b7f9f05d5a3ad96d5c23f89ab69e887d8
./N64Recomp bm64.us.toml
./RSPRecomp aspMain.toml
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBM64_TRACE_MEMORY="${BM64_TRACE_MEMORY:-OFF}" \
    -DSDL2_DIR="$tools_root/sdl2-install/lib/cmake/SDL2" \
    -DPATCHES_C_COMPILER="$llvm_root/bin/clang" -DPATCHES_LD="$llvm_root/bin/ld.lld"
cmake --build build --target PatchesBin -j "$jobs"
cmake --build build --target BM64Recompiled -j "$jobs"
