#!/bin/sh
# Requires a separate build with -DBM64_TRACE_MEMORY=ON. Never time this binary.
set -eu
cd "$(dirname "$0")/.."
unset BM64_LOOP_FRAMES BM64_CATCHUP_FRAMES BM64_BENCHMARK_FRAMES BM64_NEGATIVE_INPUT BM64_NEGATIVE_STACKS BM64_HOLD_FRAME
BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt \
BM64_CHECKPOINT_FRAME=1810 BM64_TRACE_FRAMES=800 BM64_RESULT_NAME=framebuffer-consumers \
python3 tests/run_checkpoint_probe.py
