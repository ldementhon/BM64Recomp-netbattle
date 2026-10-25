"""Compare full running-loop costs and command preparation from serial A/B runs."""
import json
import re
from pathlib import Path
from analyze_native_framebuffer import summarize
from analyze_pacing import fields, tails

ROOT = Path('experiments/results')

def identity(path):
    text = path.read_text()
    start = re.search(r'^PROBE_IMAGE: sha256=([0-9a-f]{64}) input_sha256=([0-9a-f]{64}) palette_batching=(0|1)$', text, re.M)
    end = re.search(r'^PROBE_IMAGE_UNCHANGED: sha256=([0-9a-f]{64})$', text, re.M)
    assert start and end and start[1] == end[1], 'Missing stable executable identity: '+str(path)
    return {'sha256':start[1], 'input_sha256':start[2], 'palette_batching':int(start[3])}


def prep(paths):
    rows = [fields(l) for p in paths for l in p.read_text().splitlines()
            if l.startswith('PREP_PROFILE: updates=5 ')]
    assert rows and all(r['front_calls'] == 5 and r['tiles_calls'] >= 5 for r in rows)
    assert all(r['tile_load_calls'] == r['tile_sample_calls'] == 0 for r in rows), 'Fine-grained timers contaminate comparison'
    result = {k: tails([r[k] for r in rows]) for k in rows[0] if k.endswith('_ms')}
    outside, residual = [], []
    for path in paths:
        lines = path.read_text().splitlines()
        ps = [fields(l) for l in lines if l.startswith('PROFILE: updates=5 ')]
        ns = [fields(l) for l in lines if l.startswith('NATIVE_PROFILE: updates=5 ')]
        ts = [fields(l) for l in lines if l.startswith('PREP_PROFILE: updates=5 ')]
        assert len(ps) == len(ns) == len(ts)
        for p,n,t in zip(ps,ns,ts):
            outside.append(p['graphics_worker_ms']-p['native_stage_ms']-t['front_wall_ms'])
            stages = sum(v for k,v in t.items() if k.endswith('_wall_ms') and
                         k not in ('front_wall_ms','tile_load_wall_ms','tile_sample_wall_ms'))
            waits = sum(n[k] for k in ('CPU_upload_wait_ms','texture_wait_ms','submit_ms','fence_wait_ms'))
            residual.append(p['native_stage_ms']-stages-waits)
    assert min(residual) >= -.015, 'Preparation buckets overlap or miss event alignment'
    result['native_unattributed_wall_ms'] = tails(residual)
    result['graphics_outside_native_and_front_wall_ms'] = tails(outside)
    return result

if __name__ == '__main__':
    groups, runs, pairs = {}, {}, []
    image = json.loads((ROOT/'prep-specialized-image.json').read_text())
    identities = {}
    for variant in ('before','after'):
        for i in range(1,7):
            path = ROOT/f'prep-specialized-perf-{variant}-{i}.txt'
            ident = identity(path)
            assert ident['sha256'] == image['sha256']
            assert ident['palette_batching'] == int(variant == 'after')
            identities[str(path)] = ident
    assert len({v['input_sha256'] for v in identities.values()}) == 1

    for variant in ('before', 'after'):
        for scope, count in [('clean', 3), ('all', 6)]:
            paths = [ROOT/f'prep-specialized-perf-{variant}-{i}.txt' for i in range(1, count+1)]
            groups[variant+'_'+scope], samples = summarize(paths)
            groups[variant+'_'+scope]['preparation_ms'] = prep(paths)
            assert groups[variant+'_'+scope]['rollback_events'] == 53*count
            runs.update((r['path'],r) for r in samples)
    for i in range(1,7):
        a,b = [summarize([ROOT/f'prep-specialized-perf-{v}-{i}.txt'])[0] for v in ('before','after')]
        assert a['native_readback_bytes'] == b['native_readback_bytes']
        assert a['native_noise_pixels'] == b['native_noise_pixels'] and a['native_other_pixels'] == b['native_other_pixels']
        pairs.append({'block':i,'condition':{4:'CPU 25 ms',5:'CPU 100 ms',6:'presentation 50 ms'}.get(i,'clean'),
                      'execution_after_minus_before_ms':{k:b['rollback_execution_ms'][k]-a['rollback_execution_ms'][k] for k in ('p50','p95','p99','max')},
                      'CPU_deadline_misses':[a['CPU_deadline_misses'],b['CPU_deadline_misses']],
                      'display_gaps':[a['gaps_over_25ms'],b['gaps_over_25ms']]})
    # Incremental keep gate; separate from the stricter reliable-60-Hz target.
    gate = {
        'p95_better_in_at_least_four_pairs':sum(p['execution_after_minus_before_ms']['p95']<0 for p in pairs)>=4,
        'p99_better_in_at_least_four_pairs':sum(p['execution_after_minus_before_ms']['p99']<0 for p in pairs)>=4,
        'clean_p99_improves_at_least_quarter_ms':groups['after_clean']['rollback_execution_ms']['p99']<=groups['before_clean']['rollback_execution_ms']['p99']-.25,
        'all_p99_improves_at_least_quarter_ms':groups['after_all']['rollback_execution_ms']['p99']<=groups['before_all']['rollback_execution_ms']['p99']-.25,
        'CPU_misses_do_not_increase':all(groups['after_'+s]['CPU_deadline_misses']<=groups['before_'+s]['CPU_deadline_misses'] for s in ('clean','all')),
        'no_extra_display_age':all(groups['after_'+s]['absolute_display_latency_ms'][q]<=groups['before_'+s]['absolute_display_latency_ms'][q]+1 for s in ('clean','all') for q in ('p50','p99')),
        'no_retained_latency_after_hitches':not any(h['retained_extra_latency'] for h in groups['after_all']['hitches']),
    }
    (ROOT/'prep-specialized-analysis.json').write_text(json.dumps({'groups':groups,'pairs':pairs,'runs':list(runs.values()),'identities':identities,'incremental_keep_gate':gate,'keep_gate_passed':all(gate.values())},indent=2)+'\n')
    for name,g in groups.items():
        print(name,'execution',g['rollback_execution_ms'],'misses',g['CPU_deadline_misses'],'gaps',g['gaps_over_25ms'],
              'latency',g['absolute_display_latency_ms'],'held_max',g['held_state_age_max_ms'])
    print('pairs',pairs)
