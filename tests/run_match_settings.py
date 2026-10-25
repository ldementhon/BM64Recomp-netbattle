"""Real-game check that attempted cheat edits cannot change isolated match state."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from run_independent_clients import PATTERN, ROOT

executable = ROOT / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
sha = hashlib.sha256(executable.read_bytes()).hexdigest()
environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
with tempfile.TemporaryDirectory(prefix="personal-settings-", dir=ROOT / "build/experiments") as personal:
    marker = Path(personal) / "cheats.json"
    contents = '{"moonjump":"On","infinite_lives":"On"}\n'
    marker.write_text(contents)
    environment["APP_FOLDER_PATH"] = personal
    # Refuse an existing personal directory before initializing audio/runtime.
    refused = subprocess.run([str(executable), "--checkpoint-test", "--single-battle", "--online-test"],
                             env=environment, capture_output=True, text=True, timeout=10)
    assert refused.returncode != 0 and "requires fresh settings" in refused.stderr
    rows = []
    for locked, attempt in ((True, False), (True, True), (False, False), (False, True)):
        name = "match-settings-attempt" if attempt else "match-settings-baseline"
        name += "-locked" if locked else "-offline"
        env = dict(environment, BM64_SINGLE_BATTLE="1", BM64_ONLINE_TEST="1" if locked else "0",
                   BM64_CHECKPOINT_FRAME="1980", BM64_REPLAY_LENGTH="12",
                   BM64_STOP_FRAME="2000",
                   BM64_INPUT_SCRIPT="tests/fixtures/battle-active-inputs.txt",
                   BM64_COMPARE_STATE="1", BM64_REQUIRE_BATTLE="1",
                   BM64_RESULT_NAME=name, BM64_EXPECTED_EXECUTABLE_SHA=sha)
        if attempt:
            env["BM64_TRY_SOUND"] = "1"
            if locked:
                env["BM64_TRY_CHEATS"] = "1"
        (ROOT / "experiments/results").mkdir(parents=True, exist_ok=True)
        with (ROOT / f"experiments/results/{name}-runner.txt").open("w") as log:
            subprocess.run([sys.executable, "tests/run_checkpoint_probe.py"], cwd=ROOT, env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=200)
        text = (ROOT / f"experiments/results/{name}.txt").read_text()
        history = PATTERN.findall(text)
        assert [int(row[0]) for row in history] == list(range(1981, 1993))
        assert "PASS: coordinated replay" in text
        if attempt:
            if locked:
                assert "PASS: all seven attempted mid-match cheat changes refused" in text
            balance = "locked" if locked else "changed"
            assert f"PASS: sound balance {balance}; host main volume remains adjustable" in text
        rows.append(history)
    assert rows[0] == rows[1], "attempted settings change affected online guest state"
    assert rows[2] != rows[3], "offline sound negative control failed to change guest state"
    assert marker.read_text() == contents
    assert list(Path(personal).iterdir()) == [marker]
print("PASS: online cheat/sound attempts leave full RAM/register state unchanged; offline sound change diverges; main volume adjustable; personal settings preserved")
