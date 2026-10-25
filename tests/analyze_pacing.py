"""Reconstruct displayed-state age, including held frames during gaps.

No missing presents are silently removed from deadline accounting. Retired presents
are listed separately; the displayed image continues to age until a newer one arrives.
"""
import argparse
import bisect
import json
import math
from pathlib import Path
import re
import statistics


def fields(line):
    return {k: float(v) if '.' in v else int(v) for k, v in re.findall(r'(\w+)=(-?\d+(?:\.\d+)?)', line)}


def tails(values):
    values = sorted(values)
    if not values:
        return None
    return {'n': len(values), **{name: values[min(len(values)-1, math.ceil(q*len(values))-1)]
                              for name, q in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]}}


def measured_display_period(displays, nominal):
    times = sorted(set(t for t, _ in displays))
    periods = []
    for a, b in zip(times, times[1:]):
        multiple = max(1, round((b-a)/nominal))
        estimate = (b-a)/multiple
        if abs(estimate-nominal) < nominal*.02:
            periods.append(estimate)
    assert periods, 'No physical refresh-period samples'
    return round(statistics.median(periods))


def held_ages(displays, releases, slots):
    """Age of the image actually on screen at each refresh, not just new images."""
    displays = sorted(displays)
    times = [t for t, _ in displays]
    out = []
    for slot in slots:
        pos = bisect.bisect_right(times, slot + 250_000) - 1
        if pos >= 0:
            time, frame = displays[pos]
            out.append((slot, (slot - releases[frame]) / 1e6, frame))
    return out


def recovery(ages, end, period, limit):
    # Completion of three consecutive fresh refreshes must be within four intervals.
    candidates = [a for a in ages if a[0] >= end]
    for i in range(len(candidates)-2):
        seq = candidates[i:i+3]
        if all(age <= limit for _, age, _ in seq):
            intervals = (seq[-1][0] - end) / period
            return {'refreshes': intervals, 'within_four': intervals <= 4,
                    'recovery_end_ns': seq[-1][0]}
    return {'refreshes': None, 'within_four': False, 'recovery_end_ns': None}


def analyze(path):
    text = path.read_text()
    assert 'PASS: running-loop test completed' in text, path
    lines = text.splitlines()
    frames = [fields(l) for l in lines if l.startswith('LOOP_FRAME:')]
    clock = [fields(l) for l in lines if l.startswith('LOOP_CLOCK:')][-1]
    displays = [(d['time_ns'], d['frame']) for l in lines if l.startswith('DISPLAY:')
                for d in [fields(l)] if d['time_ns']]
    retired = [fields(l)['frame'] for l in lines if l.startswith('PRESENT_RETIRED:')]
    hitches = [dict(fields(l), kind=l.split(':')[0]) for l in lines
               if l.startswith(('CPU_HITCH:', 'PRESENT_HITCH:'))]
    assert len({f['index'] for f in frames}) == len(frames)
    measured = [f for f in frames if f['index'] >= 120]
    release = {f['index']+1: f['release_ns'] for f in frames}
    simulation_period = clock['period_ns']
    period = measured_display_period(displays, simulation_period)
    # The display clock's phase is inferred from actual displayed timestamps.
    physical = sorted(t for t, f in displays if f > 60)
    assert physical
    anchor = physical[0]
    first = min(release[f['index']+1] for f in frames if f['index'] >= 60)
    last = max(max(t for t, _ in displays), max(f['finish_ns'] for f in frames))
    slots = range(anchor+math.ceil((first-anchor)/period)*period, last+1, period)
    ages = held_ages(displays, release, slots)
    warm = [age for t, age, _ in ages if release[61] <= t < release[121]]
    assert len(warm) >= 30
    warm_median = tails(warm)['p50']
    limit = min(warm_median+1.0, 2*period/1e6+1.0)
    measured_ages = [(t, age, f) for t, age, f in ages if t >= release[121]]
    results = []
    for hitch in hitches:
        r = recovery(measured_ages, hitch['end_ns'], period, limit)
        settled = [age for t, age, _ in measured_ages
                   if hitch['end_ns']+30*period <= t <= hitch['end_ns']+90*period]
        settled_tail = tails(settled)
        r['settled_age_ms'] = settled_tail
        r['settled_exceeds_age_target'] = settled_tail is None or settled_tail['p50'] > limit
        r['settled_added_age_vs_warmup_ms'] = None if settled_tail is None else settled_tail['p50']-warm_median
        r['retained_extra_latency'] = settled_tail is None or settled_tail['p50'] > warm_median+1.0
        results.append(dict(hitch, **r))
    new_displays = sorted((t, f) for t, f in displays if f > 120)
    gaps = [(b[0]-a[0])/1e6 for a, b in zip(new_displays, new_displays[1:])]
    by_frame = {}
    for time, frame in displays:
        by_frame[frame] = min(by_frame.get(frame, time), time)
    rollback = [f for f in measured if f['rollback']]
    unaffected = [f for f in measured if not any(h['start_ns'] < f['finish_ns'] and h['end_ns'] > f['wake_ns'] for h in hitches)]
    return {
        'path': str(path), 'period_ms': simulation_period/1e6, 'display_period_ms': period/1e6, 'frames': len(measured), 'rollback_events': len(rollback),
        'execution_ms': tails([f['execution_ms'] for f in measured]),
        'rollback_execution_ms': tails([f['execution_ms'] for f in rollback]),
        'wake_lateness_ms': tails([f['wake_late_ms'] for f in measured]),
        'CPU_deadline_misses': sum(f['finish_ns'] > f['deadline_ns'] for f in measured),
        'CPU_misses_without_overlapping_injection': sum(f['finish_ns'] > f['deadline_ns'] for f in unaffected),
        'CPU_lateness_ms': tails([f['deadline_lateness_ms'] for f in measured]),
        'capture_ms': tails([f['capture_ms'] for f in measured]), 'restore_ms': tails([f['restore_ms'] for f in rollback]),
        'display_deadline_misses_1ms_tolerance': sum(by_frame.get(f['index']+1, math.inf) > f['deadline_ns']+1_000_000 for f in measured),
        'undisplayed_updates': sum(f['index']+1 not in by_frame for f in measured),
        'retired_updates': sum(f > 120 for f in retired),
        'source_retired_updates': sum(f.get('source_retired',0) for f in measured),
        'absolute_display_latency_ms': tails([(t-release[f])/1e6 for t, f in new_displays]),
        'held_state_age_ms': tails([age for _, age, _ in measured_ages]),
        'display_gaps_ms': tails(gaps), 'gaps_over_25ms': sum(g > 25 for g in gaps),
        'warmup_state_age_median_ms': warm_median, 'recovery_age_limit_ms': limit,
        'recovery_target_refreshes': 4, 'recovery_requires_consecutive_refreshes': 3,
        'hitches': results,
        'checkpoint_slow': [dict(fields(l), operation=re.search(r'operation=(\w+)', l)[1]) for l in lines if l.startswith('CHECKPOINT_SLOW:')],
        'headroom_target_ms': 3.0,
        'presentation_bound': [fields(l) for l in lines if l.startswith('PRESENT_BOUND:')][-1],
    }


def self_test():
    ages = held_ages([(10_000_000, 1), (40_000_000, 4)],
                     {1: 0, 4: 30_000_000}, range(10_000_000, 61_000_000, 10_000_000))
    assert [age for _, age, _ in ages] == [10, 20, 30, 10, 20, 30]
    assert recovery([(10, 1, 1), (20, 1, 2), (30, 1, 3)], 0, 10, 2)['within_four']
    assert not recovery([(30, 1, 1), (40, 1, 2), (50, 1, 3)], 0, 10, 2)['within_four']
    assert not recovery([(10, 9, 1), (20, 9, 1), (30, 9, 1)], 0, 10, 2)['within_four']
    nominal, actual = 16_666_666, 16_667_500
    displays = [(i*actual+2*actual, i+1) for i in range(1200)]
    release = {i+1: i*nominal for i in range(1200)}
    physical = measured_display_period(displays, nominal)
    assert physical == actual
    ages = held_ages(displays, release, range(2*actual, 1202*actual, physical))
    assert max(age for _, age, _ in ages) < 35
    print('PASS: held-state age includes missing refreshes; recovery requires sustained freshness')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('logs', nargs='*', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--require-recovery', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.logs:
        data = [analyze(p) for p in args.logs]
        result = json.dumps(data, indent=2)+'\n'
        if args.output:
            args.output.write_text(result)
        else:
            print(result)
        if args.require_recovery and any(not h['within_four'] or h['retained_extra_latency'] for run in data for h in run['hitches']):
            raise SystemExit('FAIL: post-hitch recovery exceeded four refresh intervals')
