import tempfile
import unittest
from pathlib import Path

from run_independent_clients import FIRST, LAST, compare


class IndependentClientLogs(unittest.TestCase):
    def test_comparison_rejects_missing_or_incompatible_evidence(self):
        with tempfile.TemporaryDirectory() as scratch:
            first, second = Path(scratch) / "a.txt", Path(scratch) / "b.txt"
            image, inputs = "a" * 64, "b" * 64
            header = f"PROBE_IMAGE: sha256={image} input_sha256={inputs} palette_batching=0\n"
            rows = [f"COMPARABLE_V1: frame={frame} ram={'0' * 16} registers={'1' * 16}\n"
                    for frame in range(FIRST, LAST + 1)]
            footer = f"PASS: coordinated replay\nPROBE_IMAGE_UNCHANGED: sha256={image}\nRESULT: process_exit_code=0\n"
            valid = header + "".join(rows) + footer
            first.write_text(valid)
            second.write_text(valid)
            self.assertEqual(compare(first, second), [])
            second.write_text(valid.replace("registers=" + "1" * 16, "registers=" + "2" * 16, 1))
            self.assertEqual(compare(first, second), [FIRST])
            for invalid in [header + "".join(rows[1:]) + footer,
                            valid + rows[0], valid.replace("PASS: coordinated replay", "interrupted"),
                            valid.replace(image, "c" * 64), valid.replace(inputs, "d" * 64),
                            valid.replace("PROBE_IMAGE_UNCHANGED", "NOT_CHECKED")]:
                second.write_text(invalid)
                with self.assertRaises(RuntimeError):
                    compare(first, second)
            second.write_text(valid.replace(inputs, "d" * 64))
            self.assertEqual(compare(first, second, changed_input=True), [])


if __name__ == "__main__":
    unittest.main()
