#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/experiments
for optimization in 0 2; do
    clang++ -std=c++20 -Wall -Wextra -Werror -O"$optimization" \
        src/online/game_session.cpp tests/game_session.cpp -o build/experiments/game-session-test
    build/experiments/game-session-test
done
if [ "${BM64_SESSION_REAL_GAME:-0}" = 1 ]; then
    build/experiments/game-session-test \
        build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled '../Bomberman 64 (USA).z64'
fi
