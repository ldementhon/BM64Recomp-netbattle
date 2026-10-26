"""Cross-build transition checks must retain scene and full-history validation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def history(phase, image):
    first, length, scene = {
        'startup': (1300, 280, 'SINGLE_BATTLE: Player Select input loop ready at update 1374\n'
                    'PLAYER_SELECT: frame=1374 ready=1\nPLAYER_SELECT: frame=1501 ready=0\n'),
        'round': (2940, 240, 'BATTLE: frame=2940 overlay=17\n'
                  'BATTLE: frame=3060 overlay=86 stride=200 controls=' + '1:800a4600,' * 4 + ' active=4\n'),
        'match': (5380, 160, 'BATTLE_STATE: frame=5400 overlay=17 scene=20 kind=0 target=3 scores=0,0,0,3 complete=0\n'
                  'BATTLE_STATE: frame=5460 overlay=17 scene=21 kind=0 target=3 scores=0,0,0,3 complete=1\n'
                  'MATCH_COMPLETE: frame=5401 complete=1\n'),
    }[phase]
    return (f"PROBE_IMAGE: sha256={image * 64} input_sha256={'c' * 64} palette_batching=0\n"
            f"PROBE_ROM: sha256={'b' * 64}\n" + scene +
            ''.join(f"COMPARABLE_V1: frame={f} ram={'d' * 16} registers={'e' * 16}\n"
                    for f in range(first + 1, first + length + 1)) +
            f"PASS: coordinated replay\nPROBE_IMAGE_UNCHANGED: sha256={image * 64}\n"
            "RESULT: process_exit_code=0\n")


class Transitions(unittest.TestCase):
    def test_complete_and_invalid_histories(self):
        with tempfile.TemporaryDirectory() as folder:
            a, b = (Path(folder) / name for name in ('a.txt', 'b.txt'))
            for phase in ('startup', 'round', 'match'):
                a.write_text(history(phase, 'a'))
                reference = history(phase, 'f')
                for content, expected in (
                    (reference, 0),
                    (reference.replace('COMPARABLE_V1:', 'MISSING:', 1), 1),
                    (reference.replace('frame=1374 ready=1', 'frame=1374 ready=0') if phase == 'startup'
                     else reference.replace('overlay=17', 'overlay=99'), 1),
                    (reference.replace("ram=" + 'd' * 16, "ram=" + '0' * 16, 1), 1),
                    (reference.replace('process_exit_code=0', 'process_exit_code=1'), 1),
                ):
                    with self.subTest(phase=phase, expected=expected):
                        b.write_text(content)
                        result = subprocess.run([sys.executable, str(ROOT / 'tests/run_online_transition_tests.py'),
                                                 '--cross-build', phase, str(a), str(b)],
                                                capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
