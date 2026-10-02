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

    def test_trap_only_seed_becomes_a_boundary_stub(self):
        # 0x001001c8 is the first BIOS syscall: with the halt removed it now
        # translates as a stub that stops at its own entry, exactly where the
        # interpreter stops.
        result = self.run_tool("0x1001c8")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("state.set_pc(0x001001c8u);", result.stdout)

    def test_calls_translate_the_direct_call_tree(self):
        result = self.run_tool("0x10c0c0", 2000)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("inline void function_0010c0c0(ee::GuestState& state);", result.stdout)
        self.assertIn("inline void function_0044cb58(ee::GuestState& state);", result.stdout)
        self.assertIn("function_0044cb58(state);", result.stdout)
        self.assertIn("state.write_gpr64(31, 0x0010c0d0u); // link", result.stdout)

    def test_indirect_calls_become_boundaries(self):
        # 0x5a3140 reaches a jalr at 0x5a3194. The module now translates and
        # stops there with the pc at the transfer, exactly like the other
        # boundaries; the earlier rejection is gone.
        result = self.run_tool("0x5a3140", 2000)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("state.set_pc(0x005a3194u);", result.stdout)
        self.assertIn("jalr", result.stdout)

    def test_survey_reports_the_translation_outcome(self):
        result = subprocess.run([str(TOOL), str(CORE), "--survey"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("survey: entries=", result.stdout)
        self.assertIn("covered instructions:", result.stdout)
        # Rejections keep reporting their reason; the module-size policy is the
        # strongest one left in the pinned text.
        self.assertIn("reason: ", result.stdout)
        self.assertIn("call tree exceeds the function limit", result.stdout)

    def test_functions_option_bounds_the_module(self):
        # A tiny function limit makes the whole-program mode fail fast with the
        # policy message; the option parses before both modes.
        result = subprocess.run([str(TOOL), str(CORE), "--functions", "8", "--all",
                                 "200000"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("exceeds the function limit", result.stderr)

        # The same option in the normal mode keeps a small translation working.
        result = subprocess.run([str(TOOL), str(CORE), "--functions", "256", "0x577878", "64"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("inline void function_00577878", result.stdout)


if __name__ == "__main__":
    unittest.main()
