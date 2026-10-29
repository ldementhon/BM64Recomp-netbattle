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


# Baseline also replays onto check 400 after its hash starts and after it is
# acknowledged; slowhash delays every hash beyond the next check.
FLAGS = {"desync": ["desync"], "baseline": ["replaycheck"], "slowhash": ["slowhash"], "nopause": ["nopause"],
         "burst": ["burst"], "freeze": ["freeze"]}

# Mid-battle Start presses on the default stage: P2 pauses, P3 resumes, P4 pauses, P1 resumes.
# Each is on a frame where that controller has no other scripted input, which it would replace.
PAUSES = "1900 1901 1000 0 0 1\n2030 2031 1000 0 0 2\n2100 2101 1000 0 0 3\n2160 2161 1000 0 0 0\n"

# The match mode plays past the first-to-three win (guest 5403) into P1's rematch on Pyramid.
REMATCH_STOP = 4600


def histories(directory, rematch=False):
    result = []
    for slot in range(4):
        text = (directory / f"player-{slot}.log").read_text()
        rows = PATTERN.findall(text)
        expected = list(range(0, (REMATCH_STOP if rematch else 1000) + 1, 200))
        assert [int(row[0]) for row in rows] == expected, f"incomplete/duplicate history: {slot}"
        assert [int(row[1]) for row in rows] == [1374 + frame for frame in expected]
        if rematch:
            assert "MATCH_FINISHED:" not in text, "the win ended the online match"
            screens = dict(re.findall(r"NETWORK_STATE: frame=(\d+) .* overlay=(\d+)", text))
            assert screens["4000"] == "17" and screens["4200"] == "13", "Player Select did not reopen after the win"
            assert screens["4600"] == "88", "P1's rematch did not start on Pyramid"
        assert all(int(row[5]) <= 12 for row in rows), "checkpoint lifetime exceeded"
        assert "MATCH_ERROR:" not in text and "CHECKPOINT FAIL:" not in text
        result.append(rows)
    assert all([row[:4] for row in result[0]] == [row[:4] for row in other] for other in result[1:]), "real clients desynchronized"
    return result


def run(mode):
    directory = SCRATCH / f"network-rom-{mode}"
    directory.mkdir(parents=True, exist_ok=True)
    metadata = {"game": sha(GAME), "harness": sha(HARNESS), "fixture": sha(FIXTURE)}
    if mode == "match":
        metadata["finish_fixture"] = sha(ROOT / "tests/fixtures/battle-finish.txt")
        metadata["rematch_fixture"] = sha(ROOT / "tests/fixtures/battle-rematch.txt")
    environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
    with tempfile.NamedTemporaryFile(mode="w", dir=SCRATCH, suffix=".txt") as recording:
        recording.write(FIXTURE.read_text())
        if mode == "negative":
            recording.write("\n1950 1970 0000 -0.7 0 0\n")
        if mode in ("pause", "nopause"):
            recording.write("\n" + PAUSES)
        if mode == "match":
            for fixture in ("battle-finish.txt", "battle-rematch.txt"):
                recording.write("\n" + (ROOT / "tests/fixtures" / fixture).read_text())
        recording.flush()
        with (directory / "runner.log").open("w") as log:
            subprocess.run([str(HARNESS), str(GAME), str(ROOT.parent / "Bomberman 64 (USA).z64"),
                            recording.name, str(directory), "4", "0" if mode == "baseline" else "120",
                            str(REMATCH_STOP if mode == "match" else 1000)] + FLAGS.get(mode, []),
                           cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=170)
    assert sha(GAME) == metadata["game"] and sha(HARNESS) == metadata["harness"], "executable changed during run"
    if mode == "desync":
        assert "PASS: live desync reported to all 4 clients; play continued to 1000; P1 restarted the match" in (directory / "runner.log").read_text()
        for slot in range(4):
            text = (directory / f"player-{slot}.log").read_text()
            assert "NETWORK_DESYNC:" in text and "NETWORK_SECTIONS:" in text and "MATCH_ERROR:" not in text
            assert "NETWORK_STATE: frame=1000 " in text, "play stopped at the desync"
        print("PASS: desync: one client's altered replay is reported to all four games, which keep playing until P1 restarts", flush=True)
        return
    rows = histories(directory, rematch=mode == "match")
    logs = [(directory / f"player-{slot}.log").read_text() for slot in range(4)]
    if mode == "baseline":
        assert all(int(history[-1][4]) == 0 for history in rows), "baseline used prediction corrections; rerun without host contention"
        for text in logs:
            assert "NETWORK_TEST_REPLAY: frame=400 after=sealed" in text, "no replay onto a sealed check"
            assert "NETWORK_TEST_REPLAY: frame=400 after=acknowledged" in text, "no replay onto an acknowledged check"
    if mode == "slowhash":
        assert all("NETWORK_CHECK_WAIT" not in text for text in logs), "a slow hash stalled the game thread"
    if mode != "baseline":
        assert sum(int(history[-1][4]) for history in rows) > 0, "no real correction exercised"
    if mode == "pause":
        # Paused frames hold the battle, so states leave the baseline at the first check after 1900.
        for text in logs:
            assert all(f"NETWORK_STATE: frame={frame} " in text for frame in (600, 800, 1000))
    if mode in ("delayed", "negative", "slowhash", "pause", "nopause", "burst", "freeze"):
        reference = SCRATCH / "network-rom-baseline"
        assert json.loads((reference / "metadata.json").read_text()) == metadata, "baseline build or recording differs"
        original = histories(reference)[0]
        differences = [int(a[0]) for a, b in zip(original, rows[0]) if a[:4] != b[:4]]
        if mode in ("negative", "pause"):
            assert differences and differences[0] == 600, f"unexpected {mode} divergence: {differences}"
        else:
            assert not differences, f"corrected ROM differs from baseline at {differences}"
    (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"PASS: {mode}: four real ROM histories agree; corrections={[int(history[-1][4]) for history in rows]}", flush=True)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) == 2 else "all"
    modes = ("baseline", "delayed", "negative", "slowhash", "pause", "nopause", "burst", "freeze", "desync")
    if len(sys.argv) > 2 or mode not in ("all", "match") + modes:
        raise SystemExit("Usage: run_network_game.py [" + "|".join(modes + ("match",)) + "]")
    for selected in (modes if mode == "all" else (mode,)):
        run(selected)
