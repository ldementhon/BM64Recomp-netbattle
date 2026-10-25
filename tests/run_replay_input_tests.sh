#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
scratch=$(mktemp -d "${TMPDIR:-/tmp}/bm64-replay-input.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
for optimization in 0 2; do
    clang++ -std=c++20 -O"$optimization" -Wall -Wextra -Werror \
        tests/replay_input_timing.cpp -o "$scratch/replay-input"
    "$scratch/replay-input" "$scratch/inputs.txt"
done
