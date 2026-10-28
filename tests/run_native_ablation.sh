#!/bin/sh
# Balanced four-treatment comparison; run serially without builds or other game instances.
set -eu
: "${BM64_EXECUTABLE:?Set BM64_EXECUTABLE to the archived pre-removal build; current builds have removed these options}"
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/rollback-inputs.txt BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_INPUT_BUFFER=2 BM64_PROFILE=1
export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
unset BM64_LOOP_ORACLE BM64_LOOP_VERIFY BM64_TRACE_FRAMES BM64_CATCHUP_FRAMES BM64_CATCHUP_RENDER_ALL BM64_SUPPRESS_REPLAY BM64_BENCHMARK_FRAMES BM64_BENCHMARK_NO_SNAPSHOT BM64_RING_REPLAY BM64_FULL_COPY BM64_NEGATIVE_INPUT BM64_NEGATIVE_STACKS BM64_HOLD_FRAME BM64_CHECKPOINT_STALL BM64_HITCH_MS BM64_HITCH_KIND BM64_NATIVE_NO_IDLE BM64_NATIVE_LAZY_DITHER BM64_NATIVE_SHARED_UPLOADS
export BM64_LOOP_FRAMES=1200 BM64_ROLLBACK_SPACING=20 BM64_ROLLBACK_LENGTH=4
for run in 1 2 3 4; do
    case "$run" in
        1) variants='original dither both shared' ;;
        2) variants='dither shared original both' ;;
        3) variants='shared both dither original' ;;
        4) variants='both original shared dither' ;;
    esac
    for variant in $variants; do
        unset BM64_NATIVE_SHARED_UPLOADS BM64_NATIVE_LAZY_DITHER
        case "$variant" in shared|both) export BM64_NATIVE_SHARED_UPLOADS=1 ;; esac
        case "$variant" in dither|both) export BM64_NATIVE_LAZY_DITHER=1 ;; esac
        BM64_RESULT_NAME="ablation-$variant-$run" python3 tests/run_checkpoint_probe.py
    done
done
