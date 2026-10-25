"""Compare real ROM states under independent network delay and receiver loss."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
HARNESS = ROOT / "build/lobby/network_game"
FIXTURE = ROOT / "tests/fixtures/battle-active-inputs.txt"
SCRATCH = ROOT / "build/experiments"
PATTERN = re.compile(r"NETWORK_STATE: frame=(\d+) guest=(\d+) ram=([0-9a-f]{16}) registers=([0-9a-f]{16}) corrections=(\d+) max_depth=(\d+)")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def histories(directory, complete=False):
    result = []
    replay_finishes = 0
    for slot in range(4):
        text = (directory / f"player-{slot}.log").read_text()
        rows = PATTERN.findall(text)
        expected = list(range(0, 4001 if complete else 1001, 200)) + ([4029] if complete else [])
        assert [int(row[0]) for row in rows] == expected, f"incomplete/duplicate history: {slot}"
        assert [int(row[1]) for row in rows] == [1374 + frame for frame in expected]
        if complete:
            assert re.search(r"MATCH_FINISHED: match=1 frame=5403 ", text), "missing confirmed normal finish"
            replay_finishes += bool(re.search(r"MATCH_FINISHED:.* replay=1", text))
        assert all(int(row[5]) <= 12 for row in rows), "checkpoint lifetime exceeded"
        assert "MATCH_ERROR:" not in text and "CHECKPOINT FAIL:" not in text
        result.append(rows)
    assert all([row[:4] for row in result[0]] == [row[:4] for row in other] for other in result[1:]), "real clients desynchronized"
    if complete:
        assert replay_finishes > 0, "terminal boundary during replay was not exercised"
    return result


def run(mode):
    directory = SCRATCH / f"network-rom-{mode}"
    directory.mkdir(parents=True, exist_ok=True)
    metadata = {"game": sha(GAME), "harness": sha(HARNESS), "fixture": sha(FIXTURE)}
    if mode == "match":
        metadata["finish_fixture"] = sha(ROOT / "tests/fixtures/battle-finish.txt")
    environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
    with tempfile.NamedTemporaryFile(mode="w", dir=SCRATCH, suffix=".txt") as recording:
        recording.write(FIXTURE.read_text())
        if mode == "negative":
            recording.write("\n1950 1970 0000 -0.7 0 0\n")
        if mode == "match":
            recording.write("\n" + (ROOT / "tests/fixtures/battle-finish.txt").read_text())
        recording.flush()
        with (directory / "runner.log").open("w") as log:
            subprocess.run([str(HARNESS), str(GAME), str(ROOT.parent / "Bomberman 64 (USA).z64"),
                            recording.name, str(directory), "4", "0" if mode == "baseline" else "120", "0" if mode == "match" else "1000"],
                           cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=170)
    assert sha(GAME) == metadata["game"] and sha(HARNESS) == metadata["harness"], "executable changed during run"
    rows = histories(directory, complete=mode == "match")
    if mode == "baseline":
        assert all(int(history[-1][4]) == 0 for history in rows), "baseline used prediction corrections; rerun without host contention"
    if mode != "baseline":
        assert sum(int(history[-1][4]) for history in rows) > 0, "no real correction exercised"
    if mode in ("delayed", "negative"):
        reference = SCRATCH / "network-rom-baseline"
        assert json.loads((reference / "metadata.json").read_text()) == metadata, "baseline build or recording differs"
        original = histories(reference)[0]
        differences = [int(a[0]) for a, b in zip(original, rows[0]) if a[:4] != b[:4]]
        if mode == "negative":
            assert differences and differences[0] == 600, f"unexpected negative-control divergence: {differences}"
        else:
            assert not differences, f"corrected ROM differs from baseline at {differences}"
    (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"PASS: {mode}: four real ROM histories agree; corrections={[int(history[-1][4]) for history in rows]}", flush=True)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) == 2 else "all"
    if len(sys.argv) > 2 or mode not in ("all", "baseline", "delayed", "negative", "match"):
        raise SystemExit("Usage: run_network_game.py [baseline|delayed|negative|match]")
    for selected in (("baseline", "delayed", "negative") if mode == "all" else (mode,)):
        run(selected)
