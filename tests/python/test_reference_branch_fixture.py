"""Independent word/header controls for original live-reference fixtures."""

from pathlib import Path
import hashlib
import json
import subprocess
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import reference_branch_fixture as fixture


class ReferenceBranchFixtureTests(unittest.TestCase):
    def test_literal_branch_slot_words_and_destinations(self):
        for name, encoding in [("beq", 0x11000004), ("bgez", 0x05010004), ("beql", 0x51000004)]:
            for taken in [False, True]:
                words = fixture.program_words(name, taken, "syscall", 0x2F)
                self.assertEqual(words[24], encoding)
                self.assertEqual(words[25], 0x0000000C)
                self.assertEqual(words[12], 0x2403002F)
                self.assertEqual(words[13], 0x24080000 if taken else
                                 0x2408FFFF if name == "bgez" else 0x24080001)
                self.assertEqual(0x00100060 + 4 + (encoding & 0xFFFF) * 4, 0x00100074)
                self.assertEqual((words[27] & 0x03FFFFFF) * 4, 0x00100078)
                self.assertEqual(words[27], words[30])

    def test_plain_slot_and_break_are_explicit(self):
        effect = fixture.program_words("beq", True, "effect", 0x2F)
        trap = fixture.program_words("beq", True, "break", 0x2F)
        self.assertEqual(effect[25], 0x26100001)
        self.assertEqual(trap[25], 0x0000000D)
        self.assertEqual(effect[:25] + effect[26:], trap[:25] + trap[26:])

    def test_elf_header_and_load_geometry(self):
        words = fixture.program_words("beql", False, "syscall", 0x2F)
        image = fixture.build_elf(words)
        self.assertEqual(len(image), 4096 + 128)
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", image)
        self.assertEqual(header[0], b"\x7fELF\x01\x01\x01" + bytes(9))
        self.assertEqual(header[1:8], (2, 8, 1, 0x100000, 52, 0, 0x20924001))
        self.assertEqual(header[8:], (52, 32, 1, 0, 0, 0))
        self.assertEqual(struct.unpack_from("<8I", image, 52),
                         (1, 4096, 0x100000, 0x100000, 128, 0x20000, 7, 4096))
        self.assertEqual(list(struct.unpack_from("<32I", image, 4096)), words)
        self.assertEqual(image[84:4096], bytes(4096 - 84))

    def test_invalid_parameters_stop_loudly(self):
        for arguments in [("bad", True, "syscall", 0x2F), ("beq", True, "bad", 0x2F),
                          ("beq", True, "syscall", -1), ("beq", True, "syscall", 0x8000)]:
            with self.assertRaises(ValueError):
                fixture.program_words(*arguments)

    def test_setup_thread_and_plain_positive_control(self):
        words = fixture.program_words("plain", False, "syscall", 0x2F)
        self.assertEqual(words[:12], [0x3C1D01FF, 0x37BDFF00, 0x00002021,
                         0x3C050011, 0x3C060001, 0x3C070010, 0x34E78000,
                         0x3C080010, 0x35080078, 0x2403003C, 0x0000000C, 0x0040E821])
        self.assertEqual(words[24:26], [0, 0x0000000C])

    def test_plain_fixture_is_byte_pinned(self):
        image = fixture.build_elf(fixture.program_words("plain", False, "syscall", 0x2F))
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         "0078338809212e74dd6006d1f4a6a5f3ce75ca3aefc1030534e03accdd5c19ff")

    def test_cli_metadata_and_refusal_to_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "original.elf"
            command = [sys.executable, str(ROOT / "scripts/reference_branch_fixture.py"),
                       str(output), "--branch", "plain", "--taken", "no",
                       "--slot", "syscall", "--service", "0x2f"]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            original = output.read_bytes()
            metadata = json.loads(result.stdout)
            self.assertEqual(metadata["sha256"], hashlib.sha256(original).hexdigest())
            self.assertEqual(metadata["size"], 4224)
            self.assertEqual(metadata["addresses"]["branch"], "0x100060")
            repeated = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(repeated.returncode, 0)
            self.assertIn("FileExistsError", repeated.stderr)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
