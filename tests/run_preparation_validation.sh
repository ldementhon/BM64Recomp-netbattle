#!/bin/sh
# Validation of the final native-preparation decision; fresh settings per run.
set -eu
: "${BM64_EXECUTABLE:?Use the frozen original-baseline executable; the shared controller import changes legacy fixture behavior and RAM layout.}"
: "${BM64_EXPECTED_EXECUTABLE_SHA:?Specify its SHA256 from the preparation manifest.}"
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=1 BM64_INPUT_SCRIPT=tests/fixtures/rollback-inputs.txt BM64_CHECKPOINT_FRAME=1945 BM64_REQUIRE_BATTLE=1 BM64_INPUT_BUFFER=2 BM64_PROFILE=1
export BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1
unset BM64_COALESCE_PALETTE BM64_PREP_DETAIL BM64_PREP_PROFILE BM64_NATIVE_LAZY_DITHER BM64_NATIVE_SHARED_UPLOADS BM64_REUSE_SNAPSHOT_MAPPING BM64_PREDICT_DISPLAY_AGE BM64_RELEASE_LEAD_US BM64_PRESENT_TRACE BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_HITCH_MS BM64_HITCH_KIND BM64_HOLD_FRAME BM64_RESULT_PREFIX BM64_INPUT_EXTRA
export BM64_ROLLBACK_SPACING=20 BM64_ROLLBACK_LENGTH=4
case "${1:-verify}" in
perf)
    export BM64_LOOP_FRAMES=1200
    for run in 1 2 3; do
        BM64_RESULT_NAME="prep-specialized-final-perf-$run" python3 tests/run_checkpoint_probe.py
    done
    unset BM64_HITCH_MS BM64_HITCH_KIND
    export BM64_ROLLBACK_SPACING=60
    for length in 8 12; do
        BM64_ROLLBACK_LENGTH="$length" BM64_RESULT_NAME="prep-specialized-final-stress-$length" python3 tests/run_checkpoint_probe.py
    done ;;
verify)
    export BM64_LOOP_VERIFY=1 BM64_RUN_TIMEOUT=360
    for length in 4 8 12; do
        BM64_LOOP_FRAMES=1200 BM64_ROLLBACK_LENGTH="$length" BM64_HITCH_KIND=2 BM64_HITCH_MS=100 BM64_RESULT_NAME="prep-specialized-final-verify-$length" python3 tests/run_checkpoint_probe.py
    done
    BM64_LOOP_FRAMES=180 BM64_NEGATIVE_INPUT=1 BM64_RESULT_NAME=prep-specialized-final-negative-input python3 tests/run_checkpoint_probe.py
    unset BM64_LOOP_VERIFY
    BM64_RESULT_PREFIX=prep-specialized-final- sh tests/run_battle_replay.sh
    BM64_RESULT_PREFIX=prep-specialized-final- sh tests/run_catchup.sh verify ;;
broader)
    export BM64_LOOP_VERIFY=1 BM64_LOOP_FRAMES=1200 BM64_RUN_TIMEOUT=360 BM64_CHECKPOINT_FRAME=2045
    for fixture in stage-next stage-third cpu; do
        BM64_INPUT_EXTRA="tests/fixtures/rollback-$fixture.txt" BM64_RESULT_NAME="prep-specialized-final-broader-$fixture" python3 tests/run_checkpoint_probe.py
    done ;;
drained)
    export BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt
    for length in 4 8 12; do
        BM64_CATCHUP_FRAMES="$length" BM64_CATCHUP_SAMPLES=40 BM64_RESULT_NAME="prep-specialized-final-drained-$length" python3 tests/run_checkpoint_probe.py
    done ;;
*) echo 'Usage: sh tests/run_preparation_validation.sh [perf|verify|broader|drained]' >&2; exit 1 ;;
esac
