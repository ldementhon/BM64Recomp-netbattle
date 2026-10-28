"""Pool serial delivery experiments without discarding misses or held-frame age."""
import argparse
import json
from pathlib import Path
from analyze_native_framebuffer import summarize

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--prefix', default='delivery-perf')
    p.add_argument('--variants', nargs=2, default=['original', 'reuse'])
    args = p.parse_args()
    root = Path('experiments/results')
    groups, runs = {}, {}
    for variant in args.variants:
        for scope, count in [('clean', 3), ('all', 6)]:
            paths = [root/f'{args.prefix}-{variant}-{i}.txt' for i in range(1, count+1)]
            groups[variant+'_'+scope], samples = summarize(paths)
            assert groups[variant+'_'+scope]['rollback_events'] == 53*count
            runs.update((r['path'], r) for r in samples)
    for scope in ('clean', 'all'):
        a, b = [groups[v+'_'+scope] for v in args.variants]
        assert a['native_readback_bytes'] == b['native_readback_bytes']
    (root/f'{args.prefix}-analysis.json').write_text(json.dumps({'groups': groups, 'runs': list(runs.values())}, indent=2)+'\n')
    for k, r in groups.items():
        print(k, 'execution', r['rollback_execution_ms'], 'capture', r['capture_per_cycle_ms'],
              'restore', r['restore_ms'], 'misses', r['CPU_deadline_misses'], 'gaps', r['gaps_over_25ms'],
              'latency', r['absolute_display_latency_ms'], 'hitches',
              [(round(h['refreshes'], 3), h['retained_extra_latency']) for h in r['hitches']])
