"""Offline checks for the PCSX2 savestate reader, plus a local-file smoke test."""

import contextlib
import io
import struct
import sys
import tempfile
from pathlib import Path
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import pcsx2_savestate as savestate


def build_structures(pc, haystack_filler=b"\x7f" * 30):
    parts = []
    parts.append(b"BIOS" + bytes(28))
    parts.append(struct.pack("<I", 0xDEADBEEF))
    bio = b"SCPH-TEST bios desc" + bytes(256 - 19)
    parts.append(bio)
    parts.append(haystack_filler)
    parts.append(b"cpuRegs" + bytes(25))
    body = bytearray(1272)
    for index in range(32):
        struct.pack_into("<Q", body, index * 16, 0x1000 + index)
        struct.pack_into("<Q", body, index * 16 + 8, 0xFFFFFFFFFFFFFFFF)
    struct.pack_into("<Q", body, 512, 0x1111)
    struct.pack_into("<Q", body, 528, 0x2222)
    struct.pack_into("<I", body, 680, pc)
    struct.pack_into("<I", body, 544 + 12 * 4, 0x40000000)  # cp0 status
    parts.append(bytes(body))
    return b"".join(parts)


def make_savestate(directory, pc=0x00123456):
    path = Path(directory) / "test.p2s"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr(savestate.VERSION_ENTRY,
                         struct.pack("<I", 7) + b"vTest" + bytes(27))
        archive.writestr(savestate.STRUCTURES_ENTRY, build_structures(pc))
        archive.writestr(savestate.MEMORY_ENTRY, struct.pack("<II", 0xAABBCCDD, 0) + bytes(16))
    return path


class ParserTests(unittest.TestCase):
    def test_find_tag_requires_zero_padding(self):
        data = b"xxcpuRegs" + bytes(10) + b"\x99" + bytes(20) + b"yy"
        self.assertIsNone(savestate.find_tag(data, "cpuRegs"))
        tagged = b"xx" + b"cpuRegs" + bytes(25) + b"data"
        self.assertEqual(savestate.find_tag(tagged, "cpuRegs"), 2)

    def test_reads_cpu_state_from_synthetic_structures(self):
        structures = build_structures(0x00123456)
        state = savestate.read_cpu_state(structures)
        self.assertEqual(state["pc"], 0x00123456)
        self.assertEqual(state["gpr"][0], 0x1000)
        self.assertEqual(state["gpr"][31], 0x1000 + 31)
        self.assertEqual(state["hi"], 0x1111)
        self.assertEqual(state["lo"], 0x2222)
        self.assertEqual(state["cp0"]["status"], 0x40000000)

    def test_missing_tag_is_an_error(self):
        with self.assertRaises(ValueError):
            savestate.read_cpu_state(bytes(100))

    def test_pinned_timing_layout_preserves_64_bit_values(self):
        structures = bytearray(build_structures(0x005ADCE4))
        cpu_start = savestate.find_tag(structures, "cpuRegs") + 32
        # Hand-computed fixture offsets, independent of the reader constants.
        struct.pack_into("<Q", structures, cpu_start + 1088, 0x123456789ABCDEF0)
        structures += b"EE-Subsystems" + bytes(19)
        for index in range(4):
            structures += struct.pack("<6IQ", 100 + index, 0x382, 2669,
                                      0, 512, 9 + index, 0x1234567800000000 + index)
        timing = savestate.read_timing_state(structures, 0x9A590000, "v2.9.114")
        self.assertEqual(timing["ee_cycle"], 0x123456789ABCDEF0)
        self.assertEqual(timing["counters"][2], {
            "count": 102, "mode": 0x382, "target": 2669, "hold": 0,
            "rate": 512, "interrupt": 11, "start_cycle": 0x1234567800000002,
        })

    def test_timing_rejects_unverified_version(self):
        with self.assertRaisesRegex(ValueError, "not verified"):
            savestate.read_timing_state(build_structures(0), 0x9A580000, "v2.9.114")

    def test_timing_rejects_other_build_with_same_save_version(self):
        for build in ["v2.9.93", "v2.9.115", "v2.9.114-modified", ""]:
            with self.subTest(build=build), self.assertRaisesRegex(ValueError, "not verified for build"):
                savestate.read_timing_state(build_structures(0), 0x9A590000, build)

    def test_timing_rejects_missing_tag(self):
        with self.assertRaisesRegex(ValueError, "freeze tags"):
            savestate.read_timing_state(build_structures(0), 0x9A590000, "v2.9.114")

    def test_timing_rejects_truncated_counter_array(self):
        structures = build_structures(0) + b"EE-Subsystems" + bytes(19) + bytes(127)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            savestate.read_timing_state(structures, 0x9A590000, "v2.9.114")

    def test_timing_rejects_truncated_cpu_cycle(self):
        structures = b"EE-Subsystems" + bytes(19) + bytes(128) + b"cpuRegs" + bytes(25) + bytes(1095)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            savestate.read_timing_state(structures, 0x9A590000, "v2.9.114")

    def test_cli_timing_output(self):
        structures = bytearray(build_structures(0))
        struct.pack_into("<Q", structures, savestate.find_tag(structures, "cpuRegs") + 32 + 1088,
                         1646380287)
        structures += b"EE-Subsystems" + bytes(19)
        for index in range(4):
            structures += struct.pack("<6IQ", 1517, 0x382, 2669, 0, 512,
                                      index + 9, 1646380032)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "timing.p2s"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(savestate.VERSION_ENTRY, struct.pack("<I", 0x9A590000) + b"v2.9.114\0")
                archive.writestr(savestate.STRUCTURES_ENTRY, structures)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(savestate.main(["timing", str(path)]), 0)
            self.assertIn("EE cycle:     1646380287", output.getvalue())
            self.assertIn("timer 2: count=1517 mode=0x00000382", output.getvalue())
            self.assertIn("start_cycle=1646380032", output.getvalue())
            self.assertIn("raw/lazy", output.getvalue())

    def test_cli_timing_rejects_unverified_layout(self):
        with tempfile.TemporaryDirectory() as folder:
            path = make_savestate(folder)
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                self.assertEqual(savestate.main(["timing", str(path)]), 1)
            self.assertIn("not verified", error.getvalue())

    def test_reads_a_synthetic_savestate_end_to_end(self):
        with tempfile.TemporaryDirectory() as folder:
            path = make_savestate(folder, pc=0x0010011C)
            version, text = savestate.savestate_version(path)
            self.assertEqual(version, 7)
            self.assertEqual(text, "vTest")
            structures = savestate.read_entry(path, savestate.STRUCTURES_ENTRY)
            self.assertEqual(savestate.bios_description(structures), "SCPH-TEST bios desc")
            state = savestate.read_cpu_state(structures)
            self.assertEqual(state["pc"], 0x0010011C)

    def test_cli_registers_output(self):
        with tempfile.TemporaryDirectory() as folder:
            path = make_savestate(folder, pc=0x00345678)
            self.assertEqual(savestate.main(["registers", str(path), "--with-memory"]), 0)


def find_local_savestate():
    """The repository copy travels with the folder; the live emulator's
    Documents folder is the fallback for machines that have PCSX2 installed."""
    candidates = [
        ROOT / "private" / "pcsx2" / "sstates" / "SCUS-97328 (77E61C8A).09.p2s",
        Path.home() / "Documents" / "PCSX2" / "sstates" / "SCUS-97328 (77E61C8A).09.p2s",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


LOCAL_SAVESTATE = find_local_savestate()


@unittest.skipUnless(LOCAL_SAVESTATE is not None, "Requires the local GT4 menu savestate")
class LocalSavestateTests(unittest.TestCase):
    def test_menu_savestate_parses(self):
        state = savestate.read_cpu_state(savestate.read_entry(
            LOCAL_SAVESTATE, savestate.STRUCTURES_ENTRY))
        self.assertNotEqual(state["pc"], 0)
        # The menu runs game code: pc should sit inside the loaded text range.
        self.assertTrue(0x00100000 <= state["pc"] < 0x00627A14, hex(state["pc"]))


if __name__ == "__main__":
    unittest.main()
