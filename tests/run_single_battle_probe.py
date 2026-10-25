"""Bounded threaded-runtime boot check; optional screenshot for scene inspection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

root = Path(__file__).resolve().parents[1]
scratch = root / 'build/experiments'
scratch.mkdir(parents=True, exist_ok=True)
(root / 'experiments/results').mkdir(parents=True, exist_ok=True)
helper = scratch / 'capture-game-window'
capture = os.environ.get('BM64_CAPTURE') == '1'
if capture:
    subprocess.run(['swiftc', str(root/'tests/capture_game_window.swift'), '-o', str(helper)], check=True, timeout=60)
with tempfile.TemporaryDirectory(prefix='single-battle-', dir=scratch) as settings:
    shutil.copyfile(root.parent/'Bomberman 64 (USA).z64', Path(settings)/'bm64_us.z64')
    with (root/'experiments/results/single-battle-normal.txt').open('w') as log:
        process = subprocess.Popen([str(root/'build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled'), '--single-battle'],
            cwd=root, env=dict(os.environ, APP_FOLDER_PATH=settings), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        timer = threading.Timer(45, process.kill)
        timer.start()
        start = time.monotonic()
        reached = False
        try:
            for line in process.stdout:
                log.write(line)
                log.flush()
                if 'SINGLE_BATTLE: character-select boot complete' in line:
                    reached = True
                    log.write(f'RESULT: boot_seconds={time.monotonic()-start:.3f}\n')
                    time.sleep(2) # Verify the shortcut stops injecting input.
                    if process.poll() is not None:
                        raise RuntimeError('Game exited after boot')
                    if capture:
                        windows = subprocess.check_output([str(helper), str(process.pid)], text=True, timeout=5).splitlines()
                        if not windows:
                            raise RuntimeError('Game window not found')
                        subprocess.run(['screencapture', '-x', '-o', '-l', windows[0], str(scratch/'single-battle-normal.png')], check=True, timeout=5)
                    break
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
            process.wait()
        log.write(f'RESULT: reached_boot_boundary={reached}\n')
        if not reached:
            raise SystemExit('FAIL: single-battle boot did not finish')
print('PASS: normal threaded boot reached the handoff boundary and stayed running; inspect screenshot for scene identification')
