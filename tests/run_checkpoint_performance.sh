#!/bin/sh
# Run serially without full-RAM verification competing with the timed game work.
set -eu
unset BM64_LOOP_FRAMES BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_TRACE_FRAMES
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1
export BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt
export BM64_CHECKPOINT_FRAME=1945 BM64_BENCHMARK_FRAMES=600 BM64_REQUIRE_BATTLE=1
unset BM64_CATCHUP_RENDER_ALL BM64_CATCHUP_FRAMES BM64_CATCHUP_SAMPLES BM64_CATCHUP_VERIFY BM64_SUPPRESS_REPLAY BM64_NEGATIVE_STACKS BM64_NEGATIVE_INPUT BM64_HOLD_FRAME BM64_RING_REPLAY BM64_FULL_COPY BM64_BENCHMARK_NO_SNAPSHOT
BM64_BENCHMARK_NO_SNAPSHOT=1 BM64_RESULT_NAME=perf-no-snapshot python3 tests/run_checkpoint_probe.py
BM64_RESULT_NAME=perf-cow python3 tests/run_checkpoint_probe.py
BM64_RESULT_NAME=perf-cow-repeat python3 tests/run_checkpoint_probe.py
