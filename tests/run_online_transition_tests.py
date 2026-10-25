"""Full-state rewind and fresh-process comparison across player selection or a next round."""
import hashlib
import os
import re
import subprocess
import sys

from run_independent_clients import PATTERN, ROOT, metadata

executable = ROOT / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
sha = hashlib.sha256(executable.read_bytes()).hexdigest()
environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
histories = []
if len(sys.argv) != 2 or sys.argv[1] not in ("startup", "round"):
    raise SystemExit("Usage: run_online_transition_tests.py startup|round")
startup = sys.argv[1] == "startup"
prefix = "startup-transition" if startup else "round-transition"
first, length = (1380, 200) if startup else (2940, 240)
negative_kind = "INPUT" if startup else "STACKS"
for suffix in ("a", "b", "negative-" + negative_kind.lower()):
    name = prefix + "-" + suffix
    env = dict(environment, BM64_SINGLE_BATTLE="1", BM64_CHECKPOINT_FRAME=str(first),
               BM64_REPLAY_LENGTH=str(length), BM64_STOP_FRAME=str(first + length + 1), BM64_COMPARE_STATE="1",
               BM64_INPUT_SCRIPT="tests/fixtures/battle-active-inputs.txt",
               BM64_RESULT_NAME=name,
               BM64_EXPECTED_EXECUTABLE_SHA=sha)
    if startup:
        env.update(BM64_BOOT_HANDOFF_FRAME="1400", BM64_TRACE_MENU="1")
    else:
        env["BM64_INPUT_EXTRA"] = "tests/fixtures/battle-next-round.txt"
    negative = suffix.startswith("negative-")
    if negative:
        env["BM64_NEGATIVE_" + negative_kind] = "1"
    (ROOT / "experiments/results").mkdir(parents=True, exist_ok=True)
    with (ROOT / f"experiments/results/{name}-runner.txt").open("w") as log:
        subprocess.run([sys.executable, "tests/run_checkpoint_probe.py"], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=200)
    path = ROOT / f"experiments/results/{name}.txt"
    text = path.read_text()
    if startup:
        assert "BATTLE: frame=1380 overlay=13" in text
        assert re.search(r"MENU: frame=1400 process=\d+ entry=8004386c s3=-2 s4=24 ", text)
        assert re.search(r"MENU: frame=1560 process=\d+ entry=8004386c s3=0 s4=2 ", text)
    else:
        assert "BATTLE: frame=2940 overlay=17" in text, "checkpoint missed round results"
        assert re.search(r"BATTLE: frame=3060 overlay=86 stride=200 controls=(?:1:[0-9a-f]{8},){4} active=4", text), "next round did not have four humans"
    history = PATTERN.findall(text)
    assert [int(row[0]) for row in history] == list(range(first + 1, first + length + 1))
    if negative:
        assert "PASS: negative control detected divergence" in text
    else:
        assert "PASS: coordinated replay" in text
        histories.append((metadata(path), history))
    print(f"PASS: {name}", flush=True)
assert histories[0] == histories[1], "independent transition states differ"
print(f"PASS: {sys.argv[1]} transition: fresh guest states match; full-state replay passes; {negative_kind.lower()} negative control diverges")
