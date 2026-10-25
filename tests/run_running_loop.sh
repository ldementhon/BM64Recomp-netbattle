#!/bin/sh
# Keep hash validation separate from timing; timed runs must run serially.
set -eu
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/rollback-inputs.txt
export BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_INPUT_BUFFER=2
unset BM64_LOOP_ORACLE BM64_LOOP_VERIFY BM64_TRACE_FRAMES BM64_CATCHUP_FRAMES BM64_CATCHUP_RENDER_ALL BM64_SUPPRESS_REPLAY BM64_BENCHMARK_FRAMES BM64_BENCHMARK_NO_SNAPSHOT BM64_RING_REPLAY BM64_FULL_COPY BM64_NEGATIVE_INPUT BM64_NEGATIVE_STACKS BM64_HOLD_FRAME
case "${1:-verify}" in
verify)
    export BM64_LOOP_FRAMES=720 BM64_LOOP_VERIFY=1
    for variant in baseline optimized; do
        unset BM64_INCREMENTAL_WRITEBACK BM64_SKIP_HIDDEN_FENCE
        if [ "$variant" = optimized ]; then
            export BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
        fi
        for length in 4 8 12; do
            BM64_ROLLBACK_LENGTH="$length" BM64_RESULT_NAME="loop-verify-$variant-$length" python3 tests/run_checkpoint_probe.py
        done
    done
    BM64_LOOP_FRAMES=180 BM64_ROLLBACK_LENGTH=4 BM64_NEGATIVE_INPUT=1 BM64_RESULT_NAME=loop-negative-input python3 tests/run_checkpoint_probe.py
    ;;
perf)
    export BM64_LOOP_FRAMES=720 BM64_PROFILE=1
    for variant in baseline optimized; do
        unset BM64_INCREMENTAL_WRITEBACK BM64_SKIP_HIDDEN_FENCE
        if [ "$variant" = optimized ]; then
            export BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
        fi
        for length in 0 4 8 12; do
            BM64_ROLLBACK_LENGTH="$length" BM64_RESULT_NAME="loop-perf-$variant-$length" python3 tests/run_checkpoint_probe.py
        done
    done
    # Repeat the routine target across fresh processes; stress cases stay single runs.
    for repeat in 2 3; do
        unset BM64_INCREMENTAL_WRITEBACK BM64_SKIP_HIDDEN_FENCE
        BM64_ROLLBACK_LENGTH=4 BM64_RESULT_NAME="loop-perf-baseline-4-repeat-$repeat" python3 tests/run_checkpoint_probe.py
        BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 BM64_ROLLBACK_LENGTH=4 BM64_RESULT_NAME="loop-perf-optimized-4-repeat-$repeat" python3 tests/run_checkpoint_probe.py
    done
    ;;
*) echo 'Usage: sh tests/run_running_loop.sh [verify|perf]' >&2; exit 1 ;;
esac
