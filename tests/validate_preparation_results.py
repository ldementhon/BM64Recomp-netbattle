"""Correctness/coverage checks remain separate from performance acceptance."""
import json
import re
from pathlib import Path
from analyze_pacing import analyze, fields, tails
from analyze_native_preparation import identity

root = Path('experiments/results')
messages = []
image_sha=json.loads((root/'prep-specialized-image.json').read_text())['sha256']
final_image_sha=json.loads((root/'prep-specialized-final-image.json').read_text())['sha256']

def passed(name, marker='PASS:'):
    text = (root/f'{name}.txt').read_text()
    assert marker in text and 'RESULT: process_exit_code=0' in text, name
    ident=identity(root/f'{name}.txt')
    assert ident['sha256']==(final_image_sha if '-final-' in name else image_sha), name
    assert ident['palette_batching']==int('-after-' in name), name
    messages.append('PASS: '+name)
    return text

for variant in ('before','after'):
    for i in range(1,7):
        t=passed(f'prep-specialized-perf-{variant}-{i}','PASS: running-loop test completed')
        assert 'input_buffer=2' in t and 'rollbacks=53' in t
        assert int(re.search(r'PRESENT_BOUND: maximum_in_flight=(\d+)',t)[1])<=2
for i in range(1,4):
    t=passed(f'prep-specialized-final-perf-{i}','PASS: running-loop test completed')
    assert 'input_buffer=2' in t and 'rollbacks=53' in t
for n in (8,12):
    t=passed(f'prep-specialized-final-stress-{n}','PASS: running-loop test completed')
    assert 'rollbacks=18' in t and 'input_buffer=2' in t
for n in (4,8,12):
    t=passed(f'prep-specialized-final-verify-{n}','PASS: running-loop full RAM/register verification')
    assert 'rollbacks=53 wrong_predictions_corrected=53 source_retired=53' in t
    assert len(re.findall(r'^PRESENT_HITCH:',t,re.M))==4
    passed(f'prep-specialized-final-drained-{n}',f'PASS: catch-up completed 40 windows of {n} updates')
for fixture,overlay in [('stage-next',87),('stage-third',88),('cpu',86)]:
    t=passed('prep-specialized-final-broader-'+fixture,'PASS: running-loop full RAM/register verification')
    assert 'rollbacks=53 wrong_predictions_corrected=53' in t
    assert f'BATTLE: frame=2045 overlay={overlay}' in t
    if fixture=='cpu': assert re.search(r'BATTLE: frame=2045 .*controls=2:',t)
for name in ['four-player-1','four-player-4','four-player-8','four-player-12','four-player-lifecycle',
             'four-player-entry-transition','compact-ring','catchup-verify-4','catchup-verify-8','catchup-verify-12','catchup-entry']:
    passed('prep-specialized-final-'+name)
for name in ['negative-input','four-player-negative-input','four-player-transition-negative-stacks','catchup-negative-input','catchup-negative-stacks']:
    passed('prep-specialized-final-'+name,'PASS: negative control detected divergence')
count=0
for path,expected in json.loads((root/'pre-writeback-baseline-hashes.json').read_text()).items():
    p=Path(path)
    actual=re.findall(r'replay=0 frame=(\d+) ram_hash=(\w+)',p.with_name('prep-specialized-final-'+p.name).read_text())
    assert actual==[tuple(item) for item in expected],p
    count+=len(actual)
assert count==1077
messages.append(f'PASS: {count} saved pre-optimization baseline RAM hashes match')
for name in ['memory','writeback','recovery','analyzer']:
    assert 'PASS' in (root/f'prep-specialized-final-{name}-unit.txt').read_text()
assert 'PASS: 274356 whole-texture hashes match' in (root/'prep-specialized-hash-unit.txt').read_text()
assert 'PASS: startup reached 300 controller reads' in (root/'prep-shared-startup.txt').read_text()
assert 'RESULT: reached_boot_boundary=True' in (root/'prep-shared-menu.txt').read_text()
messages.append('PASS: units, ordinary startup and single-battle menu handoff')
for name, marker in [('replay','PASS: running-loop full RAM/register verification'), ('negative','PASS: negative control detected divergence')]:
    path=root/f'prep-shared-{name}.txt'
    text=path.read_text()
    assert marker in text and 'RESULT: process_exit_code=0' in text
    assert identity(path)['sha256']==json.loads((root/'prep-specialized-final-image.json').read_text())['shared_executable_sha256']
messages.append('PASS: shared controller build full-state replay and negative control')

# Preserve, do not conceal, a failing timing target.
final=json.loads((root/'prep-specialized-final-analysis.json').read_text())['clean']
gate=(final['rollback_execution_ms']['p99']<=13.667 and final['CPU_deadline_misses']==0)
messages.append(('PASS' if gate else 'FAIL')+': routine four-update headroom/deadline target')
(root/'prep-specialized-final-validation.txt').write_text('\n'.join(messages)+'\n')
print('\n'.join(messages))
