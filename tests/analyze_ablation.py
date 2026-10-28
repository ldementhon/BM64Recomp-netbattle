"""Compare independently enabled native optimizations, retaining per-run variance."""
import json
from pathlib import Path
from analyze_native_framebuffer import summarize

if __name__ == '__main__':
    root = Path('experiments/results')
    groups, runs = {}, {}
    for variant in ('original', 'dither', 'shared', 'both'):
        paths = [root / f'ablation-{variant}-{i}.txt' for i in range(1, 5)]
        groups[variant], runs[variant] = summarize(paths)
        assert groups[variant]['rollback_events'] == 212
        assert groups[variant]['cycles'] == 4320
        assert groups[variant]['native_readback_bytes'] == groups['original']['native_readback_bytes']
    paired = {}
    for variant in ('dither', 'shared', 'both'):
        paired[variant] = [
            {'block': i+1,
             'execution_median_delta_ms': r['rollback_execution_ms']['p50']-b['rollback_execution_ms']['p50'],
             'execution_p99_delta_ms': r['rollback_execution_ms']['p99']-b['rollback_execution_ms']['p99'],
             'CPU_misses_delta': r['CPU_deadline_misses']-b['CPU_deadline_misses'],
             'display_gaps_delta': r['gaps_over_25ms']-b['gaps_over_25ms']}
            for i, (b, r) in enumerate(zip(runs['original'], runs[variant]))]
    result = {'groups': groups, 'runs': runs, 'paired_deltas': paired,
              'design': 'Four fresh processes per treatment, balanced order; two-update input buffer; same executable; no injected hitches.'}
    (root/'ablation-analysis.json').write_text(json.dumps(result, indent=2)+'\n')
    for variant, r in groups.items():
        print(variant, 'execution', r['rollback_execution_ms'], 'CPU misses', r['CPU_deadline_misses'],
              'gaps', r['gaps_over_25ms'], 'display', r['absolute_display_latency_ms'])
