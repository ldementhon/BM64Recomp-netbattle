"""Real-game seat assignment, menu ownership, and rewind through battle entry."""
import hashlib
import os
import re
import subprocess
import sys
import tempfile

from run_independent_clients import PATTERN, ROOT

executable = ROOT / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
sha = hashlib.sha256(executable.read_bytes()).hexdigest()
environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}


def run(name, humans, noise, first=1300, length=800, online=True,
        recording="tests/fixtures/battle-active-inputs.txt", extra=None, stage_overlay=86):
    env = dict(environment, BM64_SINGLE_BATTLE="1", BM64_ONLINE_TEST="1" if online else "0",
               BM64_ONLINE_HUMANS=str(humans), BM64_BOOT_TO_PLAYER_SELECT="1", BM64_TRACE_MENU="1",
               BM64_INPUT_SCRIPT=recording, BM64_COMPARE_STATE="1",
               BM64_CHECKPOINT_FRAME=str(first), BM64_REPLAY_LENGTH=str(length),
               BM64_STOP_FRAME=str(first + length + 1), BM64_RESULT_NAME=name,
               BM64_EXPECTED_EXECUTABLE_SHA=sha, BM64_RUN_TIMEOUT="240")
    if noise or extra:
        env["BM64_INPUT_EXTRA"] = extra or "tests/fixtures/online-menu-interference.txt"
    with (ROOT / f"build/experiments/{name}.log").open("w") as log:
        subprocess.run([sys.executable, "tests/run_checkpoint_probe.py"], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=260)
    text = (ROOT / f"experiments/results/{name}.txt").read_text()
    rows = PATTERN.findall(text)
    assert [int(row[0]) for row in rows] == list(range(first + 1, first + length + 1))
    assert "PASS: coordinated replay" in text
    if online:
        assert "PLAYER_SELECT: frame=1374 ready=1" in text
        assert "PLAYER_SELECT: frame=1501 ready=0" in text
        controls = "1:[0-9a-f]{8}," * humans + "2:[0-9a-f]{8}," * (4 - humans)
        assert re.search(rf"BATTLE: frame=1800 overlay={stage_overlay} stride=200 controls=" + controls + " active=4", text)
    print(f"PASS: {name}", flush=True)
    return text, rows


if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1] not in ("round", "stage", "negative")):
    raise SystemExit("Usage: run_match_layouts.py [round|stage|negative]")
mode = sys.argv[1] if len(sys.argv) == 2 else "all"

if mode == "all":
    for humans in (2, 3, 4):
        _, baseline = run(f"layout-{humans}-baseline", humans, False)
        _, interference = run(f"layout-{humans}-interference", humans, True)
        assert baseline == interference, f"forbidden menu input changed the {humans}-human game"

if mode in ("all", "round"):
    with tempfile.NamedTemporaryFile(mode="w", dir=ROOT / "build/experiments", suffix=".txt") as recording:
        for fixture in ("battle-active-inputs.txt", "battle-next-round.txt"):
            recording.write((ROOT / "tests/fixtures" / fixture).read_text() + "\n")
        recording.flush()
        baseline = None
        for noise in (False, True):
            text, rows = run("layout-round-" + ("interference" if noise else "baseline"),
                             4, noise, first=2940, length=240, recording=recording.name)
            assert "BATTLE: frame=2940 overlay=17" in text
            assert re.search(r"BATTLE: frame=3060 overlay=86 stride=200 controls=(?:1:[0-9a-f]{8},){4} active=4", text)
            if baseline is not None:
                assert rows == baseline, "P2 acknowledged the round or P1 escaped the fixed preset"
            baseline = rows

# P1 picks the third stage (Pyramid, overlay 88); the same presses from P2 change nothing.
if mode in ("all", "stage"):
    _, baseline = run("layout-stage-baseline", 4, False)
    run("layout-stage-p1", 4, False, extra="tests/fixtures/single-battle-stage-third.txt", stage_overlay=88)
    _, other = run("layout-stage-p2", 4, False, extra="tests/fixtures/online-stage-other-player.txt")
    assert other == baseline, "a non-P1 player changed the stage"

# Without the online policy, the same recording must alter the guest state.
# This guards against a vacuous interference fixture.
if mode in ("all", "negative"):
    _, baseline = run("layout-offline-baseline", 4, False, length=200, online=False)
    _, interference = run("layout-offline-negative", 4, True, length=200, online=False)
    differences = [int(a[0]) for a, b in zip(baseline, interference) if a != b]
    assert differences and differences[0] == 1401
    print("PASS: offline menu interference first diverges at boundary 1401")
print("PASS: requested layout/menu checks completed")
