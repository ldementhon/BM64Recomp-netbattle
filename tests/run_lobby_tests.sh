#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cmake -S server -B build/lobby -DCMAKE_BUILD_TYPE=Release
cmake --build build/lobby -j 4
ctest --test-dir build/lobby --output-on-failure
if [ "${BM64_LOBBY_REAL_GAME:-0}" = 1 ]; then
    game=build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled
    image_sha_before=$(shasum -a 256 "$game")
    build/lobby/lobby_client "$game" '../Bomberman 64 (USA).z64' tests/fixtures/battle-active-inputs.txt
    image_sha_after=$(shasum -a 256 "$game")
    test "$image_sha_before" = "$image_sha_after"
    printf 'PASS: real-game executable unchanged: %s\n' "$image_sha_after"
fi
