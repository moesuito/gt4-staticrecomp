"""Optional M7 CLI integration checks using the pinned private CORE."""

from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "build/gt4blocks.exe"
CORE = ROOT / "private/fingerprint-check/CORE.GT4"


@unittest.skipUnless(TOOL.exists() and CORE.exists(), "Requires native build and private CORE")
class BlocksCliTests(unittest.TestCase):
    def run_tool(self, start="0x5a3140", limit="40"):
        return subprocess.run([str(TOOL), str(CORE), start, str(limit)],
                              capture_output=True, text=True)

    def test_real_block_ends_at_first_branch_with_delay_slot(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "block=0x005a3140 end=0x005a3170 instructions=12 ending=branch "
            "target_known=1 target=0x005a31b0 continuation=0x005a3170",
            result.stderr)
        self.assertIn("reason=branch", result.stderr)
        self.assertIn("delay_slot_unsupported=0", result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 12)
        self.assertIn("005a3168: 12400011  beq s2, zero, 0x005a31b0", result.stdout)
        self.assertIn("005a316c: 0080982d  daddu s3, a0, zero", result.stdout)

    def test_unsupported_start_stops_with_context(self):
        # 0x001041f4 holds an `ldl` (unaligned 64-bit load) word the model
        # still rejects; the startup prologue and COP0 now decode and run.
        result = self.run_tool("0x1041f4", "10")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("instructions=1 ending=unsupported", result.stderr)
        self.assertIn("reason=unsupported", result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertIn("unsupported", result.stdout)
        self.assertIn("68a30007", result.stdout)

    def test_bad_arguments_fail_without_listing(self):
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
