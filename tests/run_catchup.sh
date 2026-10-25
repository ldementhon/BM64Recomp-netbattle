#!/bin/sh
# Run correctness separately from timing; never run the timing cases concurrently.
set -eu
unset BM64_LOOP_FRAMES BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_TRACE_FRAMES
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt
export BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1
unset BM64_CATCHUP_FRAMES BM64_CATCHUP_SAMPLES BM64_CATCHUP_RENDER_ALL BM64_SUPPRESS_REPLAY BM64_BENCHMARK_FRAMES BM64_BENCHMARK_NO_SNAPSHOT BM64_RING_REPLAY BM64_FULL_COPY BM64_NEGATIVE_INPUT BM64_NEGATIVE_STACKS BM64_HOLD_FRAME BM64_CATCHUP_VERIFY
case "${1:-verify}" in
verify)
    for length in 4 8 12; do
        count=$(( (400 + length - 1) / length ))
        BM64_CATCHUP_FRAMES="$length" BM64_CATCHUP_SAMPLES="$count" BM64_CATCHUP_VERIFY=1 BM64_RESULT_NAME="catchup-verify-$length" python3 tests/run_checkpoint_probe.py
    done
    BM64_CATCHUP_FRAMES=12 BM64_CATCHUP_SAMPLES=1 BM64_CATCHUP_VERIFY=1 BM64_NEGATIVE_INPUT=1 BM64_RESULT_NAME=catchup-negative-input python3 tests/run_checkpoint_probe.py
    unset BM64_REQUIRE_BATTLE
    BM64_CHECKPOINT_FRAME=1810 BM64_CATCHUP_FRAMES=12 BM64_CATCHUP_SAMPLES=10 BM64_CATCHUP_VERIFY=1 BM64_RESULT_NAME=catchup-entry python3 tests/run_checkpoint_probe.py
    BM64_CHECKPOINT_FRAME=1810 BM64_REPLAY_LENGTH=120 BM64_STOP_FRAME=2500 BM64_SUPPRESS_REPLAY=1 BM64_NEGATIVE_STACKS=1 BM64_RESULT_NAME=catchup-negative-stacks python3 tests/run_checkpoint_probe.py
    ;;
perf)
    for length in 4 8 12; do
        BM64_CATCHUP_FRAMES="$length" BM64_CATCHUP_SAMPLES=100 BM64_RESULT_NAME="catchup-perf-$length" python3 tests/run_checkpoint_probe.py
    done
    for length in 4 8 12; do
        BM64_CATCHUP_FRAMES="$length" BM64_CATCHUP_SAMPLES=100 BM64_CATCHUP_RENDER_ALL=1 BM64_RESULT_NAME="catchup-control-$length" python3 tests/run_checkpoint_probe.py
    done
    ;;
*) echo 'Usage: sh tests/run_catchup.sh [verify|perf]' >&2; exit 1 ;;
esac
