"""Exercise native renderer counter restoration from an active Battle checkpoint."""
import os
from pathlib import Path
import re
import subprocess
import sys

from run_independent_clients import ROOT, metadata, records

env = {key: value for key, value in os.environ.items() if not key.startswith('BM64_')}
for key in ('BM64_EXECUTABLE', 'BM64_ROM'):
    if key in os.environ:
        env[key] = str(Path(os.environ[key]).resolve())
env.update(BM64_SINGLE_BATTLE='1', BM64_CHECKPOINT_FRAME='1940', BM64_REPLAY_LENGTH='20',
           BM64_STOP_FRAME='1961', BM64_INPUT_SCRIPT='tests/fixtures/battle-active-inputs.txt',
           BM64_REQUIRE_BATTLE='1', BM64_COMPARE_STATE='1', BM64_TRACE_RENDER_STATE='1',
           BM64_RESULT_NAME='render-checkpoint')
results = ROOT / 'experiments/results'
results.mkdir(parents=True, exist_ok=True)
with (results / 'render-checkpoint-runner.txt').open('w') as log:
    subprocess.run([sys.executable, 'tests/run_checkpoint_probe.py'], cwd=ROOT, env=env,
                   stdout=log, stderr=subprocess.STDOUT, timeout=200, check=True)
path = results / 'render-checkpoint.txt'
metadata(path)
records(path, 1941, 1960)
rows = re.findall(r'^RENDER_STATE: restore=([01]) slot=(\d+) frame=(\d+) dither=(\d+)$', path.read_text(), re.M)
assert len(rows) == 4 and [r[0] for r in rows] == ['0', '1', '1', '1'], rows
assert all(r[1:] == rows[0][1:] for r in rows), rows
assert rows[0][1] == '0' and int(rows[0][2]) > 0 and int(rows[0][3]) > 0, rows
print('PASS: active Battle renderer counters restored on all three replays; full RAM/register history matches')
