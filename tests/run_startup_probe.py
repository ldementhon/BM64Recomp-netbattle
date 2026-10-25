"""Bounded real-game startup check, using fresh settings and saves per run.

This counts controller reads, not gameplay ticks. It does not test rollback.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
rom = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root.parent / "Bomberman 64 (USA).z64"
executable = root / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
scratch = root / "build/experiments"
scratch.mkdir(parents=True, exist_ok=True)
log_path = root / "experiments/results/startup-probe.txt"
log_path.parent.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="startup-", dir=scratch) as settings:
    shutil.copyfile(rom, Path(settings) / "bm64_us.z64")
    environment = dict(os.environ, APP_FOLDER_PATH=settings)
    with log_path.open("w") as log:
        try:
            result = subprocess.run(
                [str(executable), "--smoke-test"], cwd=root, env=environment,
                stdout=log, stderr=subprocess.STDOUT, timeout=45,
            )
        except subprocess.TimeoutExpired:
            log.write("FAIL: startup exceeded 45 seconds\n")
            raise SystemExit(f"FAIL: startup exceeded 45 seconds; see {log_path}")
        log.write(f"RESULT: process_exit_code={result.returncode}\n")
    output = log_path.read_text()
    print(output)
    if result.returncode != 0 or "PASS: startup reached 300 controller reads" not in output or "initialized=1" not in output:
        raise SystemExit(f"FAIL: startup check (exit {result.returncode}); see {log_path}")
