"""Checks for the synthetic program generator and its reference model."""

import re
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import synth_programs as synth


class ModelTests(unittest.TestCase):
    def test_hand_computed_sign_rules(self):
        self.assertEqual(synth.sign_extend_16(0xFFFF), -1)
        self.assertEqual(synth.sign_extend_16(0x8000), -0x8000)
        self.assertEqual(synth.sign_extend_32(0x80000000), 0xFFFFFFFF80000000)
        self.assertEqual(synth.signed_32(0xFFFFFFFF), -1)
        self.assertEqual(synth.signed_32(1), 1)

    def test_hand_computed_encodings(self):
        # Cross-checked against the hand-verified words in the decode fixtures.
        self.assertEqual(synth.encode_i(0x09, 0, 8, 0xFFFF), 0x2408FFFF)
        self.assertEqual(synth.encode_i(0x09, 29, 29, 0xFFF0), 0x27BDFFF0)
        self.assertEqual(synth.encode_r(8, 9, 10, 0, 0x2A), 0x0109502A)
        self.assertEqual(synth.encode_r(0, 9, 8, 4, 0x00), 0x00094100)
        self.assertEqual(synth.encode_r(31, 0, 31, 0, 0x21), 0x03E0F821)

    def test_hand_computed_register_rules(self):
        machine = synth.Machine(bytes(synth.DATA_BYTES))
        machine.write_reg32(8, 0xFFFFFFFF)
        self.assertEqual(machine.read_reg64(8), 0xFFFFFFFFFFFFFFFF)
        machine.write_reg32(8, 0x80000000)
        self.assertEqual(machine.read_reg64(8), 0xFFFFFFFF80000000)
        machine.write_reg64(0, 0xFFFFFFFFFFFFFFFF)
        self.assertEqual(machine.read_reg64(0), 0)

    def test_hand_computed_memory_rules(self):
        machine = synth.Machine(bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]))
        self.assertEqual(machine.read_memory(synth.DATA_BASE, 4), 0x44332211)
        self.assertEqual(machine.read_memory(synth.DATA_BASE, 8), 0x8877665544332211)
        machine.write_memory(synth.DATA_BASE, 2, 0xABCD)
        self.assertEqual(machine.read_memory(synth.DATA_BASE, 2), 0xABCD)
        self.assertIn(synth.DATA_BASE + 1, machine.written_bytes)


class GeneratorTests(unittest.TestCase):
    def test_deterministic_for_a_seed(self):
        self.assertEqual(synth.generate(1234, 3), synth.generate(1234, 3))
        self.assertNotEqual(synth.generate(1234, 3), synth.generate(4321, 3))

    def test_committed_fixture_matches_the_generator(self):
        fixture = (ROOT / "tests/data/synth-straight.txt").read_text(encoding="utf-8")
        header = fixture.splitlines()[0]
        match = re.fullmatch(r"# synth-straight v1 seed=(\d+) count=(\d+)", header)
        self.assertIsNotNone(match, header)
        regenerated = synth.generate(int(match.group(1)), int(match.group(2)),
                                     require_full_coverage=True)
        self.assertEqual(regenerated, fixture)

    def test_generated_programs_have_expected_shape(self):
        text = synth.generate(99, 2)
        self.assertEqual(text.count("program p"), 2)
        self.assertEqual(text.count("expect_pc "), 2)
        self.assertEqual(text.count("expect r0 "), 2)  # r0 is always checked


if __name__ == "__main__":
    unittest.main()
