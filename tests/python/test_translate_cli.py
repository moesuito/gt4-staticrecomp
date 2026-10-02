"""Optional M13 translator CLI checks using the pinned private CORE."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "build/gt4translate.exe"
CORE = ROOT / "private/fingerprint-check/CORE.GT4"


@unittest.skipUnless(TOOL.exists() and CORE.exists(), "Requires native build and private CORE")
class TranslateCliTests(unittest.TestCase):
    def run_tool(self, start, limit=64, output=None):
        command = [str(TOOL), str(CORE), start, str(limit)]
        if output is not None:
            command.append(str(output))
        return subprocess.run(command, capture_output=True, text=True)

    def test_deterministic_translation_of_the_first_function(self):
        first = self.run_tool("0x577878")
        second = self.run_tool("0x577878")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertIn("inline void function_00577878(ee::GuestState& state)", first.stdout)
        self.assertIn("state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)))",
                      first.stdout)
        # The delay slot statement must appear before the return statement.
        delay = first.stdout.index("sw a2, 0x4(a0)")
        ret = first.stdout.index("return to ra")
        self.assertLess(delay, ret)

    def test_output_file_matches_stdout(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "translated.hpp"
            result = self.run_tool("0x577878", output=path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(path.read_text(encoding="utf-8"),
                             self.run_tool("0x577878").stdout)

    def test_rejects_unsupported_words(self):
        # 0x001001f8 is an `ei` (COP0) word: outside the translated subset.
        result = self.run_tool("0x1001f8")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("ERROR:", result.stderr)
        self.assertIn("unsupported", result.stderr)

    def test_calls_translate_the_direct_call_tree(self):
        result = self.run_tool("0x10c0c0", 2000)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("inline void function_0010c0c0(ee::GuestState& state);", result.stdout)
        self.assertIn("inline void function_0044cb58(ee::GuestState& state);", result.stdout)
        self.assertIn("function_0044cb58(state);", result.stdout)
        self.assertIn("state.write_gpr64(31, 0x0010c0d0u); // link", result.stdout)

    def test_rejects_indirect_calls_with_context(self):
        # 0x5a3140 jumps below its entry (now allowed and walked) and reaches
        # a jalr, which stays rejected with the exact word and address.
        result = self.run_tool("0x5a3140")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Indirect calls are not supported", result.stderr)
        self.assertIn("0x005a3194", result.stderr)  # jumps from 0x5a31cc up into startup code below the entry


if __name__ == "__main__":
    unittest.main()
