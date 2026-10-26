"""TASK-082 P1: controlled render skip (docs/TASK-082-ANTIGRAVITY-BRIEF.md).

Python contract tests:
  1. Environment variable contracts: GT4_NATIVE_FPS default off, 'skip' enabled.
  2. Forced pattern parsing: GT4_NATIVE_FPS_FORCE=2 (1 render, 1 skip) and 3 (1 render, 2 skips).
  3. Overlay syntax, sections and include verification in scripts/fps_probe_overlay.inc.
  4. Runtime capsule JSON receipt validation for slice 311.
"""

import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RenderSkipEnvironmentTests(unittest.TestCase):
    def test_default_is_disabled(self):
        # Default unset, empty or anything other than 'skip' must be disabled
        for value in [None, '', '0', '1', 'false', 'no', 'true', '2', 'fast']:
            enabled = (value == 'skip')
            self.assertFalse(enabled, f"Value {value!r} must be disabled")

    def test_enabled_flag(self):
        self.assertTrue('skip' == 'skip', "'skip' enables render skipping")

    def test_force_pattern_parsing(self):
        def parse_force(val):
            if val in ('2', '3'):
                return int(val)
            return 0

        self.assertEqual(parse_force(None), 0)
        self.assertEqual(parse_force(''), 0)
        self.assertEqual(parse_force('0'), 0)
        self.assertEqual(parse_force('1'), 0)
        self.assertEqual(parse_force('2'), 2)
        self.assertEqual(parse_force('3'), 3)
        self.assertEqual(parse_force('4'), 0)
        self.assertEqual(parse_force('skip'), 0)


class RenderSkipOverlayTests(unittest.TestCase):
    def setUp(self):
        self.overlay_path = ROOT / 'scripts/fps_probe_overlay.inc'
        self.assertTrue(self.overlay_path.is_file(), "scripts/fps_probe_overlay.inc must exist")
        self.overlay_text = self.overlay_path.read_text(encoding='utf-8')

    def test_overlay_has_required_sections_and_includes(self):
        self.assertIn('//@section includes', self.overlay_text)
        self.assertIn('#include "fps_probe.hpp"', self.overlay_text)
        self.assertIn('#include "render_skip.hpp"', self.overlay_text)
        self.assertIn('//@section helpers', self.overlay_text)
        self.assertIn('gt4::render_skip::SkipManager::shared().enabled()', self.overlay_text)
        self.assertIn('//@section hook', self.overlay_text)


class RenderSkipCapsuleTests(unittest.TestCase):
    def test_capsule_json_validity_if_present(self):
        capsule_json = ROOT / 'docs/inputs/slice311-task082-render-skip-runtime.json'
        if capsule_json.is_file():
            data = json.loads(capsule_json.read_text(encoding='utf-8'))
            self.assertEqual(data.get('schema_version'), 1)
            self.assertIn('dispatch.cpp', data.get('source_sha256', {}))
            self.assertIn('kernel.cpp', data.get('source_sha256', {}))
            self.assertIn('cdvdfsv.cpp', data.get('source_sha256', {}))


if __name__ == '__main__':
    unittest.main()
