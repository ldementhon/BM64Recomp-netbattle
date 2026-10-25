"""Bounded macOS game checkpoint test. Every run uses fresh settings and saves."""
import os
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

root = Path(__file__).resolve().parents[1]
scratch = root / 'build/experiments'
scratch.mkdir(parents=True, exist_ok=True)
name = os.environ.get('BM64_RESULT_PREFIX', '') + os.environ.get('BM64_RESULT_NAME', 'checkpoint-probe')
if not name.replace('-', '').replace('_', '').isalnum():
    raise SystemExit('Invalid result name')
log_path = root / f'experiments/results/{name}.txt'
log_path.parent.mkdir(parents=True, exist_ok=True)
capture = bool(int(os.environ.get('BM64_HOLD_FRAME', '0')))
helper = scratch / 'capture-game-window'
if capture:
    subprocess.run(['swiftc', str(root/'tests/capture_game_window.swift'), '-o', str(helper)], check=True, timeout=60)
with tempfile.TemporaryDirectory(prefix='checkpoint-', dir=scratch) as settings:
    shutil.copyfile(root.parent / 'Bomberman 64 (USA).z64', Path(settings) / 'bm64_us.z64')
    environment = dict(os.environ, APP_FOLDER_PATH=settings)
    if environment.get('BM64_INPUT_EXTRA'):
        combined = Path(settings) / 'combined-inputs.txt'
        combined.write_text(Path(environment['BM64_INPUT_SCRIPT']).read_text() + '\n' +
                            Path(environment['BM64_INPUT_EXTRA']).read_text())
        environment['BM64_INPUT_SCRIPT'] = str(combined)
    with log_path.open('w') as log:
        arguments = [os.environ.get('BM64_EXECUTABLE', str(root/'build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled')), '--checkpoint-test']
        if os.environ.get('BM64_SINGLE_BATTLE') == '1': arguments.append('--single-battle')
        if os.environ.get('BM64_ONLINE_TEST') == '1': arguments.append('--online-test')
        image_path = Path(arguments[0]).resolve()
        image_sha = hashlib.sha256(image_path.read_bytes()).hexdigest()
        expected_sha = environment.get('BM64_EXPECTED_EXECUTABLE_SHA')
        if expected_sha and image_sha != expected_sha:
            raise RuntimeError('Executable changed before launch: '+image_sha)
        input_path = environment.get('BM64_INPUT_SCRIPT')
        input_sha = hashlib.sha256(Path(input_path).read_bytes()).hexdigest() if input_path else 'none'
        log.write(f"PROBE_IMAGE: sha256={image_sha} input_sha256={input_sha} palette_batching={int('BM64_COALESCE_PALETTE' in environment)}\n")
        log.flush()
        process = subprocess.Popen(arguments,
            cwd=root, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        timer = threading.Timer(int(os.environ.get('BM64_RUN_TIMEOUT', '180')), process.kill)
        timer.start()
        try:
            for line in process.stdout:
                log.write(line)
                log.flush()
                if capture and 'holding frame' in line:
                    time.sleep(0.3)
                    window = subprocess.check_output([str(helper), str(process.pid)], text=True, timeout=5).splitlines()
                    if not window:
                        raise RuntimeError('Game window not found')
                    subprocess.run(['screencapture', '-x', '-o', '-l', window[0], str(scratch/f'{name}.png')], check=True, timeout=5)
            code = process.wait(timeout=5)
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()
        final_sha = hashlib.sha256(image_path.read_bytes()).hexdigest()
        if final_sha != image_sha:
            raise RuntimeError('Executable changed during run: '+final_sha)
        log.write(f'PROBE_IMAGE_UNCHANGED: sha256={final_sha}\n')
        log.write(f'RESULT: process_exit_code={code}\n')
output = log_path.read_text()
print(output)
expected = 'PASS: negative control detected divergence' if ('BM64_NEGATIVE_STACKS' in os.environ or 'BM64_NEGATIVE_INPUT' in os.environ) else 'PASS: coordinated replay'
if int(os.environ.get('BM64_BENCHMARK_FRAMES', '0')): expected = 'PASS: checkpoint performance run completed'
if int(os.environ.get('BM64_CATCHUP_FRAMES', '0')) and 'BM64_NEGATIVE_INPUT' not in os.environ and 'BM64_NEGATIVE_STACKS' not in os.environ: expected = 'PASS: catch-up completed'
if int(os.environ.get('BM64_TRACE_FRAMES', '0')): expected = 'PASS: framebuffer read trace completed'
if int(os.environ.get('BM64_LOOP_FRAMES', '0')) and 'BM64_NEGATIVE_INPUT' not in os.environ: expected = 'PASS: running-loop'
if int(os.environ.get('BM64_CHECKPOINT_FRAME', '0')) and expected not in output:
    raise SystemExit('FAIL: expected replay did not complete')
raise SystemExit(code)
