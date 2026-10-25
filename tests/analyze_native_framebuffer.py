"""Pool native GPU attribution and end-to-end timing without hiding missed frames."""
import argparse
import json
import re
from pathlib import Path
from analyze_pacing import analyze, fields, tails


def summarize(paths):
    runs = [analyze(p) for p in paths]
    frames, native, profiles, latency, gaps = [], [], [], [], []
    for path in paths:
        lines = path.read_text().splitlines()
        raw = [fields(l) for l in lines if l.startswith('LOOP_FRAME:')]
        frames.extend(f for f in raw if f['index'] >= 120)
        release = {f['index']+1: f['release_ns'] for f in raw}
        shown = sorted((d['time_ns'], d['frame']) for l in lines if l.startswith('DISPLAY:')
                       for d in [fields(l)] if d['time_ns'] and d['frame'] > 120)
        latency.extend((t-release[f])/1e6 for t, f in shown)
        gaps.extend((b[0]-a[0])/1e6 for a, b in zip(shown, shown[1:]))
        native.extend(fields(l) for l in lines if l.startswith('NATIVE_PROFILE: updates=5 '))
        profiles.extend(fields(l) for l in lines if l.startswith('PROFILE: updates=5 '))
    rollback = [f for f in frames if f['rollback']]
    assert native and len(native) == len(rollback), 'Missing rollback attribution'
    assert all(n['invalid'] == 0 and n['samples'] > 0 for n in native), 'Invalid Metal GPU timestamps'
    result = {
        'paths': [str(p) for p in paths], 'fresh_runs': len(paths), 'cycles': len(frames),
        'rollback_events': len(rollback),
        'rollback_execution_ms': tails([f['execution_ms'] for f in rollback]),
        'normal_execution_ms': tails([f['execution_ms'] for f in frames if not f['rollback']]),
        'wake_lateness_ms': tails([f['wake_late_ms'] for f in frames]),
        'capture_per_cycle_ms': tails([f['capture_ms'] for f in frames]),
        'restore_ms': tails([f['restore_ms'] for f in rollback]),
        'absolute_display_latency_ms': tails(latency), 'display_gap_ms': tails(gaps),
        'held_state_age_max_ms': max(r['held_state_age_ms']['max'] for r in runs),
        'hitches': [h for r in runs for h in r['hitches']],
        'native_rollback_attribution_ms': {k: tails([r[k] for r in native]) for k in native[0] if k.endswith('_ms')},
        'rollback_profile_ms': {k: tails([r[k] for r in profiles]) for k in profiles[0] if k.endswith('_ms')},
        'native_GPU_samples': sum(r['samples'] for r in native),
        'native_readback_bytes': sum(r['RAM_copy_bytes'] for r in profiles),
        'native_RAM_stored_bytes': sum(r['RAM_stored_bytes'] for r in profiles),
        'native_invalid_samples': sum(r['invalid'] for r in native),
        'native_shared_bytes': sum(r.get('shared_bytes', 0) for r in native),
        'native_shared_copies_avoided': sum(r.get('shared_copies', 0) for r in native),
        'native_noise_pixels': sum(r['noise_pixels'] for r in native),
        'native_other_pixels': sum(r['other_pixels'] for r in native),
        # Two views of the same interval; not additive cost categories.
        'absolute_GPU_timeline_closure_error_ms': tails([abs(
            r['GPU_execute_ms']+r['submit_to_GPU_ms']+r['GPU_to_callback_ms']+r['callback_to_wake_ms']
            -r['submit_ms']-r['fence_wait_ms']) for r in native]),
    }
    for key in ('CPU_deadline_misses', 'display_deadline_misses_1ms_tolerance', 'undisplayed_updates',
                'retired_updates', 'source_retired_updates', 'gaps_over_25ms'):
        result[key] = sum(r[key] for r in runs)
    return result, runs


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--require-targets', action='store_true')
    args = parser.parse_args()
    result, all_runs = {}, {}
    for variant in ('before', 'after'):
        for scope in ('clean', 'all'):
            paths = [p for p in args.logs if (m := re.fullmatch(r'native-perf-'+variant+r'-(\d+)\.txt', p.name))
                     and (scope == 'all' or int(m[1]) <= 3)]
            if paths:
                result[variant+'_'+scope], runs = summarize(sorted(paths))
                all_runs.update((r['path'], r) for r in runs)
    for scope in ('clean', 'all'):
        if 'before_'+scope in result and 'after_'+scope in result:
            before, after = result['before_'+scope], result['after_'+scope]
            assert before['rollback_events'] == after['rollback_events']
            assert before['native_readback_bytes'] == after['native_readback_bytes'], 'Native readback volume changed'
    for p in args.logs:
        if p.name.startswith('native-stress-'):
            all_runs[str(p)] = analyze(p)
    args.output.write_text(json.dumps({'groups': result, 'runs': list(all_runs.values())}, indent=2)+'\n')
    if args.require_targets:
        after, before = result['after_clean'], result['before_clean']
        if (after['CPU_deadline_misses'] or after['rollback_execution_ms']['p99'] > 13.667 or
                after['absolute_display_latency_ms']['p99'] > before['absolute_display_latency_ms']['p99']+1):
            raise SystemExit('FAIL: clean four-update headroom/deadline/display-latency targets not all met')
