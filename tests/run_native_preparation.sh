#!/bin/sh
# Serial, same-binary comparisons; normal update and checkpoint work included.
set -eu
: "${BM64_EXPECTED_EXECUTABLE_SHA:?Specify the frozen executable SHA256 from prep-specialized-image.json.}"
: "${BM64_EXECUTABLE:?The palette-batching trial was removed. Rebuild with the archived palette-batching patches and point BM64_EXECUTABLE at that executable.}"
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/rollback-inputs.txt BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_INPUT_BUFFER=2 BM64_PROFILE=1
export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
unset BM64_RELEASE_LEAD_US BM64_PRESENT_TRACE BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_HITCH_MS BM64_HITCH_KIND BM64_HOLD_FRAME BM64_RESULT_PREFIX BM64_INPUT_EXTRA BM64_PREP_DETAIL BM64_COALESCE_PALETTE
export BM64_PREP_PROFILE=1 BM64_ROLLBACK_SPACING=20 BM64_ROLLBACK_LENGTH=4
case "${1:-perf}" in
perf)
    export BM64_LOOP_FRAMES=1200
    for run in 1 2 3 4 5 6; do
        unset BM64_HITCH_MS BM64_HITCH_KIND
        case "$run" in
            4) export BM64_HITCH_KIND=1 BM64_HITCH_MS=25 ;;
            5) export BM64_HITCH_KIND=1 BM64_HITCH_MS=100 ;;
            6) export BM64_HITCH_KIND=2 BM64_HITCH_MS=50 ;;
        esac
        order='before after'
        case "$run" in 2|4|6) order='after before' ;; esac
        for variant in $order; do
            unset BM64_COALESCE_PALETTE
            if [ "$variant" = after ]; then export BM64_COALESCE_PALETTE=1; fi
            BM64_RESULT_NAME="prep-specialized-perf-$variant-$run" python3 tests/run_checkpoint_probe.py
        done
    done ;;
quick)
    export BM64_COALESCE_PALETTE=1 BM64_LOOP_VERIFY=1 BM64_RUN_TIMEOUT=360
    BM64_LOOP_FRAMES=180 BM64_RESULT_NAME=prep-specialized-quick-verify python3 tests/run_checkpoint_probe.py
    BM64_LOOP_FRAMES=180 BM64_NEGATIVE_INPUT=1 BM64_RESULT_NAME=prep-specialized-quick-negative python3 tests/run_checkpoint_probe.py ;;
*) echo 'Usage: sh tests/run_native_preparation.sh [perf|quick]' >&2; exit 1 ;;
esac
