#!/bin/sh
# Existing fully drained catch-up test, with stage counters and a paired A/B run.
set -eu
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt
export BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_PROFILE=1 BM64_CATCHUP_SAMPLES=40
unset BM64_LOOP_FRAMES BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_TRACE_FRAMES BM64_CATCHUP_RENDER_ALL BM64_SUPPRESS_REPLAY BM64_BENCHMARK_FRAMES BM64_BENCHMARK_NO_SNAPSHOT BM64_RING_REPLAY BM64_FULL_COPY BM64_NEGATIVE_INPUT BM64_NEGATIVE_STACKS BM64_HOLD_FRAME BM64_CATCHUP_VERIFY
for variant in baseline optimized; do
    unset BM64_INCREMENTAL_WRITEBACK BM64_SKIP_HIDDEN_FENCE
    if [ "$variant" = optimized ]; then
        export BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
    fi
    for length in 4 8 12; do
        BM64_CATCHUP_FRAMES="$length" BM64_RESULT_NAME="drained-profile-$variant-$length" python3 tests/run_checkpoint_probe.py
    done
done
# Repeat the stress case to retain an independent view of checkpoint tail latency.
BM64_CATCHUP_FRAMES=12 BM64_RESULT_NAME=drained-profile-optimized-12-repeat python3 tests/run_checkpoint_probe.py
