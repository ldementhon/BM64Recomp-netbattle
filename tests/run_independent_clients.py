"""Compare fresh processes, without transferring any native checkpoints.

This checks all RAM and canonical guest registers, not portable native stacks.
Use --cross-build first-log second-log to compare two explicit build identities
with matching ROM/input identities. This diagnostic does not admit builds online.
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


def metadata(path):
    text = path.read_text()
    matches = re.findall(r"^PROBE_IMAGE: sha256=([0-9a-f]{64}) input_sha256=([0-9a-f]{64}) ", text, re.M)
    unchanged = re.findall(r"^PROBE_IMAGE_UNCHANGED: sha256=([0-9a-f]{64})$", text, re.M)
    if len(matches) != 1 or unchanged != [matches[0][0]]:
        raise RuntimeError(f"Missing executable/input identity or executable changed: {path}")
    return matches[0]


def compare(first, second, *, changed_input=False, cross_build=False):
    image_a, input_a = metadata(first)
    image_b, input_b = metadata(second)
    if (not cross_build and image_a != image_b) or (not changed_input and input_a != input_b):
        raise RuntimeError("Comparison requires the same executable and input recording")
    if cross_build:
        if changed_input:
            raise RuntimeError("Cross-build comparison requires identical input")
        roms = [re.findall(r"^PROBE_ROM: sha256=([0-9a-f]{64})$", path.read_text(), re.M)
                for path in (first, second)]
        if len(roms[0]) != 1 or roms[0] != roms[1]:
            raise RuntimeError("Cross-build comparison requires matching ROM identities")
    a, b = records(first), records(second)
    return [frame for frame in a if a[frame] != b[frame]]


def run(name, executable_sha, extra=None):
    # No inherited performance/negative-control switches may change the fixture.
    env = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
    for key in ("BM64_EXECUTABLE", "BM64_ROM"):
        if key in os.environ:
            env[key] = str(Path(os.environ[key]).resolve())
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
    cross_build = len(sys.argv) == 4 and sys.argv[1] == "--cross-build"
    if len(sys.argv) == 3 or cross_build:
        first, second = map(Path, sys.argv[-2:])
        differences = compare(first, second, cross_build=cross_build)
        if differences:
            raise SystemExit(f"FAIL: first independent guest-state divergence at {differences[0]}")
        print("PASS: complete independent guest-state histories match (native continuations not certified)")
        if cross_build:
            print(f"Compared executable SHA-256 pair: {metadata(first)[0]} {metadata(second)[0]}")
    elif len(sys.argv) == 1:
        (ROOT / "experiments/results").mkdir(parents=True, exist_ok=True)
        executable = Path(os.environ.get("BM64_EXECUTABLE", ROOT / (
            "build/BM64Recompiled.exe" if sys.platform == "win32" else
            "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled")))
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
        differences = compare(first, changed, changed_input=True)
        if not differences or differences[0] <= 1950:
            raise SystemExit("FAIL: changed-input control failed or diverged before changed input")
        print(f"PASS: independent guest-state histories match; changed input detected at {differences[0]}")
    else:
        raise SystemExit("Usage: run_independent_clients.py [--cross-build] [first-log second-log]")
