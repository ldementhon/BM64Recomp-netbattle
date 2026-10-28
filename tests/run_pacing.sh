#!/bin/sh
# All timing cases are serial, use fresh processes, and retain a two-update buffer.
set -eu
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/rollback-inputs.txt BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_INPUT_BUFFER=2 BM64_PROFILE=1
export BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
unset BM64_LOOP_ORACLE BM64_LOOP_VERIFY BM64_TRACE_FRAMES BM64_CATCHUP_FRAMES BM64_CATCHUP_RENDER_ALL BM64_SUPPRESS_REPLAY BM64_BENCHMARK_FRAMES BM64_BENCHMARK_NO_SNAPSHOT BM64_RING_REPLAY BM64_FULL_COPY BM64_NEGATIVE_INPUT BM64_NEGATIVE_STACKS BM64_HOLD_FRAME BM64_CHECKPOINT_STALL BM64_PACING BM64_BOUNDED_PRESENT BM64_INLINE_UPLOADS BM64_SKIP_EMPTY_SUBMISSIONS BM64_HITCH_MS BM64_HITCH_KIND
case "${1:-verify}" in
verify)
    export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_LOOP_VERIFY=1 BM64_ROLLBACK_SPACING=20 BM64_RUN_TIMEOUT=360
    for length in 4 8 12; do
        BM64_LOOP_FRAMES=1200 BM64_ROLLBACK_LENGTH="$length" BM64_HITCH_KIND=2 BM64_HITCH_MS=100 BM64_RESULT_NAME="pacing-verify-$length" python3 tests/run_checkpoint_probe.py
    done
    BM64_LOOP_FRAMES=180 BM64_ROLLBACK_LENGTH=4 BM64_NEGATIVE_INPUT=1 BM64_RESULT_NAME=pacing-negative-input python3 tests/run_checkpoint_probe.py
    ;;
perf)
    export BM64_LOOP_FRAMES=1200 BM64_ROLLBACK_SPACING=20 BM64_ROLLBACK_LENGTH=4
    for variant in before after; do
        unset BM64_PACING BM64_BOUNDED_PRESENT BM64_INLINE_UPLOADS BM64_SKIP_EMPTY_SUBMISSIONS
        if [ "$variant" = after ]; then export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1; fi
        for run in 1 2 3 4 5 6; do
            unset BM64_HITCH_MS BM64_HITCH_KIND
            case "$run" in
                4) export BM64_HITCH_KIND=1 BM64_HITCH_MS=25 ;;
                5) export BM64_HITCH_KIND=1 BM64_HITCH_MS=100 ;;
                6) export BM64_HITCH_KIND=2 BM64_HITCH_MS=50 ;;
            esac
            BM64_RESULT_NAME="pacing-perf-$variant-$run" python3 tests/run_checkpoint_probe.py
        done
    done
    unset BM64_HITCH_MS BM64_HITCH_KIND
    export BM64_ROLLBACK_SPACING=60
    for length in 8 12; do
        BM64_ROLLBACK_LENGTH="$length" BM64_RESULT_NAME="pacing-stress-$length" python3 tests/run_checkpoint_probe.py
    done
    ;;
drained)
    unset BM64_LOOP_FRAMES
    export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt
    for length in 4 8 12; do
        BM64_CATCHUP_FRAMES="$length" BM64_CATCHUP_SAMPLES=40 BM64_RESULT_NAME="pacing-drained-$length" python3 tests/run_checkpoint_probe.py
    done
    ;;
*) echo 'Usage: sh tests/run_pacing.sh [verify|perf|drained]' >&2; exit 1 ;;
esac
