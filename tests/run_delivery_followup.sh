#!/bin/sh
set -eu
: "${BM64_EXECUTABLE:?Historical experiment: set BM64_EXECUTABLE to the archived build documented in optimization-decisions-and-delivery.md}"
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/rollback-inputs.txt BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_INPUT_BUFFER=2 BM64_PROFILE=1
export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 BM64_PRESENT_TRACE=1
unset BM64_NATIVE_LAZY_DITHER BM64_NATIVE_SHARED_UPLOADS BM64_REUSE_SNAPSHOT_MAPPING BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_HITCH_MS BM64_HITCH_KIND BM64_RELEASE_LEAD_US
export BM64_ROLLBACK_SPACING=20 BM64_ROLLBACK_LENGTH=4
case "${1:-perf}" in
phase)
    for lead in 0 -4000 -8000 -12000; do
        BM64_LOOP_FRAMES=480 BM64_RELEASE_LEAD_US="$lead" BM64_RESULT_NAME="delivery-later-$lead" python3 tests/run_checkpoint_probe.py
    done ;;
perf)
    export BM64_LOOP_FRAMES=1200
    for run in 1 2 3 4 5 6; do
        unset BM64_HITCH_MS BM64_HITCH_KIND
        case "$run" in
            4) export BM64_HITCH_KIND=1 BM64_HITCH_MS=25 ;;
            5) export BM64_HITCH_KIND=1 BM64_HITCH_MS=100 ;;
            6) export BM64_HITCH_KIND=2 BM64_HITCH_MS=50 ;;
        esac
        variants='original reuse'
        case "$run" in 2|4|6) variants='reuse original' ;; esac
        for variant in $variants; do
            unset BM64_REUSE_SNAPSHOT_MAPPING
            if [ "$variant" = reuse ]; then export BM64_REUSE_SNAPSHOT_MAPPING=1; fi
            BM64_RESULT_NAME="delivery-perf-$variant-$run" python3 tests/run_checkpoint_probe.py
        done
    done ;;
*) echo 'Usage: sh tests/run_delivery_followup.sh [phase|perf]' >&2; exit 1 ;;
esac
