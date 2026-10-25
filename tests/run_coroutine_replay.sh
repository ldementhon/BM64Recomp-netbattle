#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/experiments
for optimization in 0 2; do
    echo "Optimization: -O$optimization"
    clang++ -std=c++20 -O"$optimization" -g \
        -Ilib/N64ModernRuntime/N64Recomp/include \
        -Ilib/N64ModernRuntime/librecomp/include \
        -Ilib/N64ModernRuntime/ultramodern/include \
        tests/coroutine_replay.cpp -o "build/experiments/coroutine_replay_O$optimization"
    "build/experiments/coroutine_replay_O$optimization"
done
