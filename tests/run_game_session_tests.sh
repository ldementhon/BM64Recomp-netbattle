#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/experiments
for optimization in 0 2; do
    clang++ -std=c++20 -Wall -Wextra -Werror -O"$optimization" \
        src/online/game_session.cpp tests/game_session.cpp -o build/experiments/game-session-test
    build/experiments/game-session-test
    clang++ -std=c++20 -Wall -Wextra -Werror -O"$optimization" \
        tests/match_channel.cpp -o build/experiments/match-channel-test
    build/experiments/match-channel-test
done
if [ "${BM64_SESSION_REAL_GAME:-0}" = 1 ]; then
    game=build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled
    image_sha_before=$(shasum -a 256 "$game")
    build/experiments/game-session-test \
        "$game" '../Bomberman 64 (USA).z64'
    image_sha_after=$(shasum -a 256 "$game")
    test "$image_sha_before" = "$image_sha_after"
    printf 'PASS: real-game executable unchanged: %s\n' "$image_sha_after"
fi
