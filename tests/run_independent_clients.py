"""Compare fresh processes, without transferring any native checkpoints.

This checks all RAM and canonical guest registers, not portable native stacks.
Run on the same tested build/platform; export logs for a second-machine comparison.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIRST, LAST = 1599, 2345
PATTERN = re.compile(r"^COMPARABLE_V1: frame=(\d+) ram=([0-9a-f]{16}) registers=([0-9a-f]{16})$", re.M)


def records(path):
    text = path.read_text()
    rows = PATTERN.findall(text)
    if [int(row[0]) for row in rows] != list(range(FIRST, LAST + 1)):
        raise RuntimeError(f"Incomplete/duplicate comparable history: {path}")
    if "PASS: coordinated replay" not in text or "RESULT: process_exit_code=0" not in text:
        raise RuntimeError(f"Replay did not finish: {path}")
    return {int(frame): (ram, registers) for frame, ram, registers in rows}


def compare(first, second):
    a, b = records(first), records(second)
    return [frame for frame in a if a[frame] != b[frame]]


def run(name, executable_sha, extra=None):
    # No inherited performance/negative-control switches may change the fixture.
    env = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
    env.update(BM64_SINGLE_BATTLE="1", BM64_CHECKPOINT_FRAME=str(FIRST - 1),
               BM64_REPLAY_LENGTH=str(LAST - FIRST + 1), BM64_STOP_FRAME=str(LAST + 1),
               BM64_INPUT_SCRIPT="tests/fixtures/battle-active-inputs.txt",
               BM64_COMPARE_STATE="1", BM64_RESULT_NAME=name,
               BM64_EXPECTED_EXECUTABLE_SHA=executable_sha)
    if extra:
        env["BM64_INPUT_EXTRA"] = str(extra)
    output = ROOT / "experiments/results" / f"{name}-runner.txt"
    with output.open("w") as log:
        subprocess.run([sys.executable, "tests/run_checkpoint_probe.py"], cwd=ROOT,
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=200)
    path = ROOT / "experiments/results" / f"{name}.txt"
    records(path)
    print(f"PASS: {name}: {LAST - FIRST + 1} comparable boundaries and full-state replay", flush=True)
    return path


if __name__ == "__main__":
    if len(sys.argv) == 3:
        differences = compare(Path(sys.argv[1]), Path(sys.argv[2]))
        if differences:
            raise SystemExit(f"FAIL: first independent guest-state divergence at {differences[0]}")
        print("PASS: complete independent guest-state histories match (native continuations not certified)")
    elif len(sys.argv) == 1:
        (ROOT / "experiments/results").mkdir(parents=True, exist_ok=True)
        executable = ROOT / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
        sha = hashlib.sha256(executable.read_bytes()).hexdigest()
        first = run("independent-a", sha)
        second = run("independent-b", sha)
        differences = compare(first, second)
        if differences:
            raise SystemExit(f"FAIL: first independent guest-state divergence at {differences[0]}")
        with tempfile.TemporaryDirectory(prefix="independent-", dir=ROOT / "build/experiments") as scratch:
            extra = Path(scratch) / "changed-input.txt"
            extra.write_text("1950 1970 0000 -0.7 0 0\n")
            changed = run("independent-negative", sha, extra)
        differences = compare(first, changed)
        if not differences or differences[0] <= 1950:
            raise SystemExit("FAIL: changed-input control failed or diverged before changed input")
        print(f"PASS: independent guest-state histories match; changed input detected at {differences[0]}")
    else:
        raise SystemExit("Usage: run_independent_clients.py [first-log second-log]")
