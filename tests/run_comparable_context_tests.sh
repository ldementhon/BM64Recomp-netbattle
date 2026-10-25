#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/experiments
for optimization in 0 2; do
    clang++ -std=c++20 -Wall -Wextra -Werror -O"$optimization" \
        -I lib/N64ModernRuntime/N64Recomp/include tests/comparable_context.cpp \
        -o build/experiments/comparable-context-test
    build/experiments/comparable-context-test
done
