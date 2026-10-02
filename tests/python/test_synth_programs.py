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


class BranchingGeneratorTests(unittest.TestCase):
    def test_committed_branching_fixture_matches_the_generator(self):
        fixture = (ROOT / "tests/data/synth-branching.txt").read_text(encoding="utf-8")
        header = fixture.splitlines()[0]
        match = re.fullmatch(r"# synth-branching v1 seed=(\d+) count=(\d+)", header)
        self.assertIsNotNone(match, header)
        regenerated = synth.generate_branching(int(match.group(1)), int(match.group(2)),
                                               require_full_coverage=True)
        self.assertEqual(regenerated, fixture)

    def test_branching_programs_terminate_and_expose_steps(self):
        text = synth.generate_branching(11, 30, require_full_coverage=True)
        self.assertEqual(text.count("program b"), 30)
        self.assertEqual(text.count("steps "), 30)
        self.assertEqual(text.count("expect r0 "), 30)

    def test_hand_computed_taken_branch_keeps_the_delay_slot(self):
        machine = synth.Machine(bytes(synth.DATA_BYTES))
        effects = {}
        target = synth.CODE_BASE + 12
        effects[synth.CODE_BASE + 0] = synth.conditional_branch_effect(
            lambda m: m.read_reg64(0) == m.read_reg64(0), False, False, target)
        effects[synth.CODE_BASE + 4] = synth.plain_step(synth.addiu_effect(0, 1, 1))
        effects[synth.CODE_BASE + 8] = synth.plain_step(synth.addiu_effect(0, 2, 2))
        effects[synth.CODE_BASE + 12] = synth.plain_step(synth.addiu_effect(0, 3, 3))
        steps = synth.simulate(machine, effects, synth.CODE_BASE + 16)
        self.assertEqual(steps, 3)
        self.assertEqual(machine.read_reg64(1), 1)  # the delay slot ran
        self.assertEqual(machine.read_reg64(2), 0)  # skipped by the branch
        self.assertEqual(machine.read_reg64(3), 3)

    def test_hand_computed_likely_not_taken_nulls_the_delay_slot(self):
        machine = synth.Machine(bytes(synth.DATA_BYTES))
        effects = {}
        target = synth.CODE_BASE + 12
        effects[synth.CODE_BASE + 0] = synth.conditional_branch_effect(
            lambda m: False, False, True, target)
        effects[synth.CODE_BASE + 4] = synth.plain_step(synth.addiu_effect(0, 1, 1))
        effects[synth.CODE_BASE + 8] = synth.plain_step(synth.addiu_effect(0, 2, 2))
        effects[synth.CODE_BASE + 12] = synth.plain_step(synth.addiu_effect(0, 3, 3))
        steps = synth.simulate(machine, effects, synth.CODE_BASE + 16)
        self.assertEqual(steps, 3)
        self.assertEqual(machine.read_reg64(1), 0)  # nullified
        self.assertEqual(machine.read_reg64(2), 2)  # continues after the skip
        self.assertEqual(machine.read_reg64(3), 3)


if __name__ == "__main__":
    unittest.main()
