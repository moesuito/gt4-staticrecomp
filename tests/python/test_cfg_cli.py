"""Optional M7 CFG CLI integration checks using the pinned private CORE."""

from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "build/gt4cfg.exe"
CORE = ROOT / "private/fingerprint-check/CORE.GT4"


@unittest.skipUnless(TOOL.exists() and CORE.exists(), "Requires native build and private CORE")
class CfgCliTests(unittest.TestCase):
    def run_tool(self, start="0x5a3140", limit="200"):
        return subprocess.run([str(TOOL), str(CORE), start, str(limit)],
                              capture_output=True, text=True)

    def test_real_traversal_starts_with_the_known_block(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertGreaterEqual(len(lines), 2)
        self.assertIn(
            "block=0x005a3140 end=0x005a3170 instructions=12 ending=branch "
            "target_known=1 target=0x005a31b0 continuation=0x005a3170 reason=branch "
            "successors=0x005a31b0,0x005a3170",
            lines[0])
        self.assertIn("cfg blocks=", result.stderr)
        self.assertIn("limited=", result.stderr)

    def test_unsupported_seed_is_a_single_open_block(self):
        # 0x001001f8 holds an `ei` (COP0) word the model still rejects; the
        # startup prologue at 0x00100008 now decodes and walks further.
        result = self.run_tool("0x1001f8", "10")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1)
        self.assertIn("ending=unsupported", lines[0])
        self.assertIn("successors=none", lines[0])
        self.assertIn("cfg blocks=1", result.stderr)

    def test_bad_arguments_fail_without_output(self):
        for start, limit in [("-1", "1"), ("0x100001", "1"), ("0x5a3140", "0"),
                             ("0x617a14", "1")]:
            with self.subTest(start=start, limit=limit):
                result = self.run_tool(start, limit)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertIn("ERROR:", result.stderr)
        result = subprocess.run([str(TOOL)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
