"""TASK-074 L2: the --linux-surfaces port derivation (X11/Wayland attach), on synthetic and, when present, real sources."""

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import prepare_external_vulkan as port  # noqa: E402


class LinuxSurfaceHookTests(unittest.TestCase):
    def test_hooks_are_unique_and_reversible_on_a_synthetic_source(self):
        source = ''.join('// before\n' + anchor + '// after\n' for anchor, _ in port.LINUX_SURFACE_HOOKS)
        derived = port.apply_linux_surface_hooks(source)
        self.assertIn('g_vulkan.attach_x11(target.display, target.window', derived)
        self.assertIn('g_vulkan.attach_wayland(target.display, target.surface', derived)
        self.assertNotIn('reinterpret_cast<HWND>', derived)
        self.assertNotIn('Win32 presentations', derived)

    def test_missing_or_duplicated_anchor_is_refused(self):
        anchors = [anchor for anchor, _ in port.LINUX_SURFACE_HOOKS]
        with self.assertRaisesRegex(ValueError, 'anchor changed'):
            port.apply_linux_surface_hooks(anchors[1])  # the attach anchor is missing
        with self.assertRaisesRegex(ValueError, 'anchor changed'):
            port.apply_linux_surface_hooks(anchors[0] + anchors[0] + anchors[1])

    def test_gs_stream_window_hook_takes_linux_windows(self):
        hooked = port.apply_gs_stream_hooks(''.join(anchor + '// filler\n' for anchor, _ in port.GS_STREAM_HOOKS))
        linux = port.apply_linux_gs_stream_hooks(hooked)
        self.assertIn('attach_linux_window(kind, display, window, surface, width, height)', linux)
        self.assertNotIn('kind == 3', linux)
        with self.assertRaisesRegex(ValueError, 'anchor changed'):
            port.apply_linux_gs_stream_hooks(linux)  # already rewritten: the Win32 anchor is gone

    @unittest.skipUnless((ROOT / 'private/references/ps2-web-recomp/runtime/src/gs/gpu/gpu.cpp').exists(),
                         'needs the pinned donor in private/references')
    def test_real_derivation_changes_only_the_surface_attach(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'private') as folder:
            default, linux = Path(folder) / 'default', Path(folder) / 'linux'
            port.prepare(default)
            port.prepare(linux, linux_surfaces=True)
            for name in ('gpu.hpp', 'gs.cpp', 'main.cpp'):
                self.assertEqual((default / name).read_bytes(), (linux / name).read_bytes(), name)
            default_gpu = (default / 'gpu.cpp').read_text(encoding='utf-8')
            linux_gpu = (linux / 'gpu.cpp').read_text(encoding='utf-8')
            self.assertEqual(port.apply_linux_surface_hooks(default_gpu), linux_gpu)
            self.assertIn('reinterpret_cast<HWND>', default_gpu)
            hooked, both = Path(folder) / 'hooked', Path(folder) / 'both'
            port.prepare(hooked, gs_stream=True)
            port.prepare(both, gs_stream=True, linux_surfaces=True)
            hooked_gs = (hooked / 'gs.cpp').read_text(encoding='utf-8')
            self.assertEqual(port.apply_linux_gs_stream_hooks(hooked_gs), (both / 'gs.cpp').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
