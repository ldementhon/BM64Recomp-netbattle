"""Keep the cross-build diagnostic strict about provenance and complete history."""
from pathlib import Path
import tempfile
import unittest

from run_independent_clients import FIRST, LAST, compare


def history(image="a", rom="b", inputs="c"):
    return (f"PROBE_IMAGE: sha256={image * 64} input_sha256={inputs * 64} palette_batching=0\n"
            f"PROBE_ROM: sha256={rom * 64}\n" +
            "".join(f"COMPARABLE_V1: frame={frame} ram={'d' * 16} registers={'e' * 16}\n"
                    for frame in range(FIRST, LAST + 1)) +
            f"PASS: coordinated replay\nPROBE_IMAGE_UNCHANGED: sha256={image * 64}\n"
            "RESULT: process_exit_code=0\n")


class Comparison(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.first = Path(self.temporary.name) / "first.log"
        self.second = Path(self.temporary.name) / "second.log"
        self.first.write_text(history())
        self.second.write_text(history(image="f"))

    def test_explicit_cross_build_only(self):
        with self.assertRaises(RuntimeError):
            compare(self.first, self.second)
        self.assertEqual(compare(self.first, self.second, cross_build=True), [])

    def test_mismatched_or_missing_provenance(self):
        for content in (history(image="f", rom="c"), history(image="f", inputs="b"),
                        history(image="f").replace(f"PROBE_ROM: sha256={'b' * 64}\n", ""),
                        history(image="f").replace(f"PROBE_IMAGE_UNCHANGED: sha256={'f' * 64}",
                                                   f"PROBE_IMAGE_UNCHANGED: sha256={'a' * 64}"),
                        history(image="f") + f"PROBE_ROM: sha256={'b' * 64}\n",
                        history(image="f") + f"PROBE_IMAGE_UNCHANGED: sha256={'f' * 64}\n"):
            with self.subTest(content=content[:100]):
                self.second.write_text(content)
                with self.assertRaises(RuntimeError):
                    compare(self.first, self.second, cross_build=True)

    def test_incomplete_duplicate_and_failed_histories(self):
        row = f"COMPARABLE_V1: frame={FIRST} ram={'d' * 16} registers={'e' * 16}\n"
        for content in (history(image="f").replace(row, ""), history(image="f") + row,
                        history(image="f").replace("process_exit_code=0", "process_exit_code=1"),
                        history(image="f").replace("PASS: coordinated replay", "FAIL: replay")):
            self.second.write_text(content)
            with self.assertRaises(RuntimeError):
                compare(self.first, self.second, cross_build=True)

    def test_divergence_is_reported(self):
        self.second.write_text(history(image="f").replace(
            f"frame={FIRST + 10} ram={'d' * 16}", f"frame={FIRST + 10} ram={'0' * 16}"))
        self.assertEqual(compare(self.first, self.second, cross_build=True), [FIRST + 10])

    def test_changed_input_cannot_relax_cross_build_check(self):
        with self.assertRaises(RuntimeError):
            compare(self.first, self.second, cross_build=True, changed_input=True)


if __name__ == "__main__":
    unittest.main()
