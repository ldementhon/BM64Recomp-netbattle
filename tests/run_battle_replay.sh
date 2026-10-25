#!/bin/sh
# From the checkout root; the runner supplies isolated saves and a 180s deadline.
set -eu
unset BM64_LOOP_FRAMES BM64_LOOP_VERIFY BM64_LOOP_ORACLE BM64_TRACE_FRAMES
cd "$(dirname "$0")/.."
export BM64_SINGLE_BATTLE=${BM64_SINGLE_BATTLE:-1}
export BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt
export BM64_STOP_FRAME=2500
export BM64_REQUIRE_BATTLE=1
unset BM64_CATCHUP_RENDER_ALL BM64_CATCHUP_FRAMES BM64_CATCHUP_SAMPLES BM64_CATCHUP_VERIFY BM64_SUPPRESS_REPLAY BM64_NEGATIVE_STACKS BM64_NEGATIVE_INPUT BM64_HOLD_FRAME BM64_RING_REPLAY BM64_BENCHMARK_FRAMES BM64_BENCHMARK_NO_SNAPSHOT BM64_FULL_COPY
for length in 1 4 8 12; do
    BM64_CHECKPOINT_FRAME=1980 BM64_REPLAY_LENGTH="$length" BM64_RESULT_NAME="four-player-$length" python3 tests/run_checkpoint_probe.py
done
BM64_CHECKPOINT_FRAME=1945 BM64_REPLAY_LENGTH=400 BM64_RESULT_NAME=four-player-lifecycle python3 tests/run_checkpoint_probe.py
BM64_CHECKPOINT_FRAME=1980 BM64_REPLAY_LENGTH=12 BM64_NEGATIVE_INPUT=1 BM64_RESULT_NAME=four-player-negative-input python3 tests/run_checkpoint_probe.py
unset BM64_REQUIRE_BATTLE
BM64_CHECKPOINT_FRAME=1810 BM64_REPLAY_LENGTH=120 BM64_RESULT_NAME=four-player-entry-transition python3 tests/run_checkpoint_probe.py
BM64_CHECKPOINT_FRAME=1810 BM64_REPLAY_LENGTH=120 BM64_NEGATIVE_STACKS=1 BM64_RESULT_NAME=four-player-transition-negative-stacks python3 tests/run_checkpoint_probe.py
python3 - <<'PY'
import re, os
from pathlib import Path

def baseline(name):
    prefix = os.environ.get('BM64_RESULT_PREFIX', '')
    return dict(re.findall(r'replay=0 frame=(\d+) ram_hash=(\w+)', Path(f'experiments/results/{prefix}{name}.txt').read_text()))

long_run = baseline('four-player-lifecycle')
for length in (1, 4, 8, 12):
    short_run = baseline(f'four-player-{length}')
    if len(short_run) != length or any(long_run.get(frame) != value for frame, value in short_run.items()):
        raise SystemExit(f'FAIL: independent baseline mismatch for window {length}')
print('PASS: independent-process baseline RAM hashes match at all overlapping frames')
PY

BM64_CHECKPOINT_FRAME=1945 BM64_REPLAY_LENGTH=400 BM64_REQUIRE_BATTLE=1 BM64_RING_REPLAY=1 BM64_RESULT_NAME=compact-ring python3 tests/run_checkpoint_probe.py
