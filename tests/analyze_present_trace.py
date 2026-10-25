"""Attribute displayed latency without treating the GPU fence as display completion."""
import argparse
import json
from pathlib import Path
from analyze_pacing import analyze, fields, tails

def trace(path):
    lines = path.read_text().splitlines()
    frames = {f['index']+1: f for l in lines if l.startswith('LOOP_FRAME:') for f in [fields(l)]}
    displays = {d['frame']: d['time_ns'] for l in lines if l.startswith('DISPLAY:') for d in [fields(l)]}
    records = [fields(l) for l in lines if l.startswith('PRESENT_TRACE:')]
    result = analyze(path)
    result['display_callback_delay_ms'] = tails([(d['callback_ns']-d['time_ns'])/1e6
        for l in lines if l.startswith('DISPLAY:') for d in [fields(l)]
        if d['frame'] > 120 and d.get('callback_ns') and d['time_ns']])
    result['presentation_stages_ms'] = {}
    for kind in ('normal', 'rollback'):
        samples = []
        for t in records:
            if t['frame'] <= 120 or t['frame'] not in displays:
                continue
            f = frames[t['frame']]
            if bool(f['rollback']) != (kind == 'rollback'):
                continue
            times = [f['release_ns'], t['queue_ns'], t['workload_ready_ns'], t['slot_ready_ns'],
                     t['acquired_ns'], t['draw_submitted_ns'], t['draw_done_ns'], t['present_called_ns'], displays[t['frame']]]
            assert times == sorted(times), (path, t['frame'], times)
            samples.append([(b-a)/1e6 for a, b in zip(times, times[1:])])
        names = ('release_to_queue', 'workload_wait', 'display_slot_wait', 'prepare_and_acquire',
                 'draw_encoding', 'draw_submit_and_fence', 'pacing_and_present_prepare', 'present_call_to_display')
        result['presentation_stages_ms'][kind] = {name: tails([s[i] for s in samples]) for i, name in enumerate(names)}
    return result

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('logs', nargs='+', type=Path)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    args.output.write_text(json.dumps([trace(path) for path in args.logs], indent=2)+'\n')
