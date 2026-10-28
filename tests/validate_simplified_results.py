"""Check completion and coverage separately from the intentionally unmet timing gate."""
import json
import re
from pathlib import Path

root = Path('experiments/results')
messages = []

def passed(name, assertion='PASS:'):
    text = (root/f'{name}.txt').read_text()
    assert assertion in text and 'RESULT: process_exit_code=0' in text, name
    messages.append('PASS: '+name)
    return text

for variant in ('original', 'dither', 'shared', 'both'):
    for run in range(1, 5):
        passed(f'ablation-{variant}-{run}', 'PASS: running-loop test completed')
for variant in ('original', 'reuse'):
    for run in range(1, 7):
        passed(f'delivery-perf-{variant}-{run}', 'PASS: running-loop test completed')
for run in range(1, 7):
    t = passed(f'simplified-perf-{run}', 'PASS: running-loop test completed')
    assert 'input_buffer=2' in t and 'rollbacks=53' in t
    assert int(re.search(r'PRESENT_BOUND: maximum_in_flight=(\d+)', t)[1]) <= 2
for length in (8, 12):
    passed(f'simplified-stress-{length}', 'PASS: running-loop test completed')
for length in (4, 8, 12):
    t = passed(f'simplified-verify-{length}', 'PASS: running-loop full RAM/register verification')
    assert 'rollbacks=53 wrong_predictions_corrected=53 source_retired=53' in t
    assert len(re.findall(r'^PRESENT_HITCH:', t, re.M)) == 4
    passed(f'simplified-drained-{length}', f'PASS: catch-up completed 40 windows of {length} updates')
for fixture, overlay in [('stage-next', 87), ('stage-third', 88), ('cpu', 86)]:
    t = passed(f'simplified-broader-{fixture}', 'PASS: running-loop full RAM/register verification')
    assert 'rollbacks=53 wrong_predictions_corrected=53' in t
    assert f'BATTLE: frame=2045 overlay={overlay}' in t
    if fixture == 'cpu':
        assert re.search(r'BATTLE: frame=2045 .*controls=2:', t)
for name in ['four-player-1', 'four-player-4', 'four-player-8', 'four-player-12',
             'four-player-lifecycle', 'four-player-entry-transition', 'compact-ring',
             'catchup-verify-4', 'catchup-verify-8', 'catchup-verify-12', 'catchup-entry']:
    passed('simplified-'+name)
for name in ['negative-input', 'four-player-negative-input', 'four-player-transition-negative-stacks',
             'catchup-negative-input', 'catchup-negative-stacks']:
    passed('simplified-'+name, 'PASS: negative control detected divergence')
count = 0
for path, expected in json.loads((root/'pre-writeback-baseline-hashes.json').read_text()).items():
    p = Path(path)
    actual = re.findall(r'replay=0 frame=(\d+) ram_hash=(\w+)', p.with_name('simplified-'+p.name).read_text())
    assert actual == [tuple(item) for item in expected], p
    count += len(actual)
assert count == 1077
baseline = f'PASS: all {count} saved pre-optimization baseline RAM hashes match the simplified path\n'
(root/'simplified-baseline-equivalence.txt').write_text(baseline)
messages.append(baseline.strip())
for name in ['memory', 'writeback', 'recovery', 'analyzer']:
    assert 'PASS' in (root/f'simplified-{name}-unit.txt').read_text()
messages.append('PASS: memory, writeback, recovery and analyzer units')
assert 'PASS: startup reached 300 controller reads' in (root/'startup-probe.txt').read_text()
assert 'RESULT: reached_boot_boundary=True' in (root/'single-battle-normal.txt').read_text()
messages.append('PASS: ordinary startup and single-battle menu handoff')
assert 'FAIL:' in (root/'simplified-target-check.txt').read_text()
messages.append('TIMING GATE REMAINS FAILED: full-state correctness does not imply reliable 60 Hz rollback')
(root/'simplified-validation.txt').write_text('\n'.join(messages)+'\n')
print('\n'.join(messages))
