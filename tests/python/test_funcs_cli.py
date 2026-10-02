"""Optional M8 CLI integration checks using the pinned private CORE."""

from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "build/gt4funcs.exe"
CORE = ROOT / "private/fingerprint-check/CORE.GT4"


@unittest.skipUnless(TOOL.exists() and CORE.exists(), "Requires native build and private CORE")
class FunctionMapCliTests(unittest.TestCase):
    def run_tool(self, start="0x5a3140", max_functions="50", max_blocks="500"):
        return subprocess.run([str(TOOL), str(CORE), start, str(max_functions), str(max_blocks)],
                              capture_output=True, text=True)

    def test_real_map_starts_with_entry_and_seed(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertGreaterEqual(len(lines), 3)
        self.assertTrue(lines[0].startswith(
            "function=0x00100008 evidence=elf-entry blocks=1 instructions=1"), lines[0])
        self.assertIn(
            "function=0x005a3140 evidence=seed blocks=15 instructions=71",
            lines[1])
        self.assertIn("call_targets=0x005b78a0", lines[1])
        self.assertTrue(any("function=0x005b78a0 evidence=direct-call" in line
                            for line in lines[2:]), result.stdout)
        self.assertIn("funcmap functions=", result.stderr)

    def test_function_cap_reports_pending_work(self):
        result = self.run_tool("0x5a3140", "1", "500")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1)
        self.assertIn("function=0x00100008 evidence=elf-entry", lines[0])
        self.assertIn("pending=1", result.stderr)
        self.assertIn("limited=1", result.stderr)

    def test_bad_arguments_fail_without_output(self):
        for start, max_functions, max_blocks in [
                ("-1", "1", "1"), ("0x100001", "1", "1"), ("0x5a3140", "0", "1"),
                ("0x5a3140", "1", "0"), ("0x617a14", "1", "1")]:
            with self.subTest(start=start, max_functions=max_functions, max_blocks=max_blocks):
                result = self.run_tool(start, max_functions, max_blocks)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertIn("ERROR:", result.stderr)
        result = subprocess.run([str(TOOL)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
