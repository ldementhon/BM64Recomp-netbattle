"""Final clean/stress/drained confirmation, distinct from the paired comparison."""
import json
from pathlib import Path
from analyze_native_framebuffer import summarize
from analyze_pacing import analyze, fields, tails
from analyze_native_preparation import identity

root=Path('experiments/results')
paths=[root/f'prep-specialized-final-perf-{i}.txt' for i in range(1,4)]
clean,runs=summarize(paths)
stress=[analyze(root/f'prep-specialized-final-stress-{n}.txt') for n in (8,12)]
drained={}
for n in (4,8,12):
    path=root/f'prep-specialized-final-drained-{n}.txt'
    lines=path.read_text().splitlines()
    assert f'PASS: catch-up completed 40 windows of {n} updates' in path.read_text()
    ps=[fields(l) for l in lines if l.startswith('PROFILE: updates=')]
    cs=[fields(l) for l in lines if l.startswith('PROFILE: capture_ms=')]
    total=[fields(l)['total_ms'] for l in lines if l.startswith('CATCHUP: sample=')]
    assert len(ps)==len(cs)==len(total)==40
    # Terms are printed rounded to .001 ms. This reconstructs work excluding final wait.
    work=[a['graphics_fence_ms']+b['capture_ms']+b['restore_ms']+b['simulation_runtime_residual_ms'] for a,b in zip(ps,cs)]
    drained[n]={'total_ms':tails(total),'work_excluding_final_wait_ms_rounded':tails(work),'identity':identity(path)}
(root/'prep-specialized-final-analysis.json').write_text(json.dumps({'clean':clean,'runs':runs,'stress':stress,'drained':drained},indent=2)+'\n')
print('Final clean',clean['rollback_execution_ms'],'CPU misses',clean['CPU_deadline_misses'],'gaps',clean['gaps_over_25ms'])
for r in stress: print('Stress',r['path'],r['rollback_execution_ms'],'CPU misses',r['CPU_deadline_misses'])
print('Drained',drained)
