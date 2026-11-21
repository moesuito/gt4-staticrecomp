"""VU1 static image and entry policy (slice 155). Synthetic encrypted blobs only; no game data.

Two controls read the private pins and skip when those files are absent: the image pin and the
agreement of the full table with the slice154 table (same key and identity digest per entry).
"""
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import vu1_image as image_tool

ROOT = Path(__file__).resolve().parents[2]


def lower_word(major, it=0, is_=0, low11=0):
    return ((major & 0x7F) << 25) | ((it & 15) << 16) | ((is_ & 15) << 11) | (low11 & 0x7FF)


def put_word(image, at, value):
    image[at:at + 4] = value.to_bytes(4, 'little')


class KeystreamTests(unittest.TestCase):
    def test_keystream_advances_before_each_byte(self):
        # Literal vector from state 0x32277070: the first four advanced states, low byte.
        self.assertEqual(image_tool.decrypt_blob(bytes(4)), bytes([0x31, 0x56, 0xEF, 0x4C]))

    def test_decrypt_is_its_own_inverse(self):
        plain = bytes((index * 37 + 11) & 0xFF for index in range(256))
        self.assertEqual(image_tool.decrypt_blob(image_tool.decrypt_blob(plain)), plain)


class ImageBuildTests(unittest.TestCase):
    def synthetic_elf(self, plain_blob):
        encrypted = image_tool.decrypt_blob(plain_blob)
        return bytes(image_tool.ELF_OFFSET) + encrypted + bytes(64)

    def test_image_is_blob_plus_tail_and_snapshots_control_it(self):
        plain = bytes((index * 13 + 5) & 0xFF for index in range(image_tool.BLOB_BYTES))
        tail = bytes(range(1, 17))
        snapshot = plain + tail
        image, facts = image_tool.build_image(self.synthetic_elf(plain), [('a.bin', snapshot), ('b.bin', snapshot)])
        self.assertEqual(image, snapshot)
        self.assertEqual(facts['snapshots_matching'], 2)
        self.assertEqual(facts['tail_hex'], tail.hex())

    def test_pinned_tail_builds_only_the_pinned_image(self):
        # TASK-074: without snapshots the tail comes from the manifest, and blob and image must both equal their pins.
        import hashlib
        plain = bytes((index * 7 + 3) & 0xFF for index in range(image_tool.BLOB_BYTES))
        tail = bytes(16)
        pins = {'blob_sha256': hashlib.sha256(plain).hexdigest(), 'tail_hex': tail.hex(),
                'image': {'sha256': hashlib.sha256(plain + tail).hexdigest()}}
        image, facts = image_tool.build_image_with_pinned_tail(self.synthetic_elf(plain), pins)
        self.assertEqual(image, plain + tail)
        self.assertEqual(facts['snapshots_matching'], 0)
        changed = bytearray(plain)
        changed[7] ^= 1
        with self.assertRaisesRegex(ValueError, 'blob differs from the pin'):
            image_tool.build_image_with_pinned_tail(self.synthetic_elf(bytes(changed)), pins)
        with self.assertRaisesRegex(ValueError, 'differs from the image pin'):
            image_tool.build_image_with_pinned_tail(self.synthetic_elf(plain), {**pins, 'tail_hex': ('01' + '00' * 15)})
        with self.assertRaisesRegex(ValueError, 'wrong length'):
            image_tool.build_image_with_pinned_tail(self.synthetic_elf(plain), {**pins, 'tail_hex': '00'})

    def test_snapshot_that_differs_from_the_blob_is_refused(self):
        plain = bytes(image_tool.BLOB_BYTES)
        wrong = bytearray(plain + bytes(16))
        wrong[100] ^= 1
        with self.assertRaisesRegex(ValueError, 'differs from the decrypted blob'):
            image_tool.build_image(self.synthetic_elf(plain), [('bad.bin', bytes(wrong))])

    def test_disagreeing_tails_are_refused(self):
        plain = bytes(image_tool.BLOB_BYTES)
        first = plain + bytes(16)
        second = plain + bytes([1]) + bytes(15)
        with self.assertRaisesRegex(ValueError, 'disagree on the tail'):
            image_tool.build_image(self.synthetic_elf(plain), [('a.bin', first), ('b.bin', second)])

    def test_wrong_size_and_empty_corpus_are_refused(self):
        plain = bytes(image_tool.BLOB_BYTES)
        with self.assertRaisesRegex(ValueError, 'not 16384'):
            image_tool.build_image(self.synthetic_elf(plain), [('short.bin', plain)])
        with self.assertRaisesRegex(ValueError, 'no VU1 snapshots'):
            image_tool.build_image(self.synthetic_elf(plain), [])


class EntryPolicyTests(unittest.TestCase):
    def authored_image(self):
        image = bytearray(image_tool.IMAGE_BYTES)
        # Conditional branch at 0x0020 with offset +3: target 0x0020 + 8 + 24 = 0x0040.
        put_word(image, 0x20, lower_word(0x28, low11=3))
        # Branch-and-link at 0x0100 with offset 0: return 0x0110, target 0x0108.
        put_word(image, 0x100, lower_word(0x21))
        # IADDIU vi1 = vi0 + 0x123 at 0x0200: constant 0x123 gives a JR target 0x0918.
        put_word(image, 0x200, lower_word(0x08, it=1, is_=0, low11=0x123))
        # E-bit at 0x0300 (upper word): the program ends after the delay slot, next entry 0x0310.
        put_word(image, 0x304, 0x40000000)
        # Jump-register at 0x0400: the code after its delay slot starts a block at 0x0410.
        put_word(image, 0x400, lower_word(0x24))
        return bytes(image)

    def test_targeted_policy_covers_each_rule(self):
        entries = image_tool.targeted_entries(self.authored_image(), table_entries=[0x0050])
        self.assertEqual(entries, [0x0000, 0x0030, 0x0040, 0x0050, 0x0108, 0x0110, 0x0310, 0x0410, 0x0918])

    def test_all_policy_is_every_aligned_address(self):
        entries = image_tool.all_entries()
        self.assertEqual(len(entries), image_tool.IMAGE_BYTES // 8)
        self.assertEqual(entries[0], 0)
        self.assertEqual(entries[-1], image_tool.IMAGE_BYTES - 8)

    def test_branch_offset_is_signed(self):
        # Offset -1 at pc 0x0010 points back to 0x0010 (pc + 8 - 8).
        self.assertEqual(image_tool.branch_target(lower_word(0x28, low11=0x7FF), 0x10), 0x10)

    def test_entry_list_is_written_once_in_upper_hex(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'entries.txt'
            image_tool.write_entry_list(path, [0x0, 0x0040, 0x3FF8])
            self.assertEqual(path.read_text(encoding='utf-8'), '0000\n0040\n3FF8\n')
            with self.assertRaises(FileExistsError):
                image_tool.write_entry_list(path, [0x0])

    def test_fresh_directory_rules(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / 'out'
            image_tool.fresh_directory(target)
            (target / 'kept.txt').write_text('x', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'fresh or empty'):
                image_tool.fresh_directory(target)


class PinnedControlTests(unittest.TestCase):
    @unittest.skipUnless((ROOT / 'docs/inputs/slice155-vu1-image.json').exists() and
                         (ROOT / 'private/slice155-vu1-image/vu1_image.bin').exists(),
                         'private slice155 image not present')
    def test_built_image_matches_its_pin(self):
        pins = json.loads((ROOT / 'docs/inputs/slice155-vu1-image.json').read_text(encoding='utf-8'))
        self.assertEqual(image_tool.sha256_file(ROOT / 'private/slice155-vu1-image/vu1_image.bin'), pins['image']['sha256'])

    @unittest.skipUnless((ROOT / 'private/slice154-vu1-flags-gen/manifest.json').exists() and
                         (ROOT / 'private/slice155-vu1-full-gen/manifest.json').exists(),
                         'private VU1 tables not present')
    def test_full_table_keeps_identity_of_the_slice154_table(self):
        old = json.loads((ROOT / 'private/slice154-vu1-flags-gen/manifest.json').read_text(encoding='utf-8'))['programs']
        new = {(p['entry']): p for p in json.loads((ROOT / 'private/slice155-vu1-full-gen/manifest.json').read_text(encoding='utf-8'))['programs']}
        for program in old:
            self.assertIn(program['entry'], new)
            self.assertEqual(new[program['entry']]['key'], program['key'])
            self.assertEqual(new[program['entry']]['digest_sha256'], program['digest_sha256'])


    @unittest.skipUnless((ROOT / 'private/slice155-vu1-full-gen/manifest.json').exists() and
                         (ROOT / 'private/slice155-vu1-image/vu1_image.bin').exists(),
                         'private slice155 table or image not present')
    def test_python_keys_match_the_generated_table(self):
        image = (ROOT / 'private/slice155-vu1-image/vu1_image.bin').read_bytes()
        manifest = json.loads((ROOT / 'private/slice155-vu1-full-gen/manifest.json').read_text(encoding='utf-8'))
        for program in manifest['programs']:
            self.assertEqual(f"0x{image_tool.program_key_of(image, program['entry']):016X}", program['key'])


def manifest_with(programs):
    return {'programs': programs}


def program_row(entry, key='0x0000000000000001', digest='ab'):
    return {'entry': entry, 'key': key, 'digest_sha256': digest, 'image_flags_used': True, 'image_status_used': False,
            'mac_mode': 'observed', 'mac_writers': 1, 'mac_sites': 1, 'mac_reader_sites': 0, 'clip_writers': 0,
            'clip_sites': 0, 'clip_reader_sites': 0, 'fallback_sites': 0, 'upper_nop_sites': 0, 'budget_heads': 1,
            'reachable_instructions': 4, 'exit_pcs': [], 'status': 'emitted', 'function': f'vu1_{entry:04X}'}


class PolicyTests(unittest.TestCase):
    def test_targeted_rows_inside_full_table_compare_equal(self):
        full = manifest_with([program_row(0), program_row(8), program_row(16)])
        targeted = manifest_with([program_row(8), program_row(16)])
        checked, problems = image_tool.compare_tables(targeted, full)
        self.assertEqual((checked, problems), (2, []))

    def test_differing_digest_or_missing_entry_is_reported(self):
        full = manifest_with([program_row(0), program_row(8, digest='cd')])
        targeted = manifest_with([program_row(8), program_row(16)])
        _, problems = image_tool.compare_tables(targeted, full)
        self.assertEqual(len(problems), 2)
        self.assertTrue(any('digest_sha256' in problem for problem in problems))
        self.assertTrue(any('entry 16 missing' in problem for problem in problems))

    def test_flag_elision_counts_of_a_table(self):
        full = manifest_with([program_row(0), program_row(8)])
        counts = image_tool.elision_counts(full)
        self.assertEqual((counts['programs'], counts['emitted'], counts['image_flags_used'], counts['interpreter_exit_sites']), (2, 2, 2, 0))

    def test_all_policy_is_every_eight_byte_address(self):
        entries = image_tool.all_entries()
        self.assertEqual(len(entries), 2048)
        self.assertEqual(entries[:3], [0, 8, 16])
        self.assertEqual(entries[-1], 16376)

    def test_split_table_covers_all_entries_when_present(self):
        # Private control for slice157: the 2,048-row table, its policy label and the 128 parts it records.
        path = ROOT / 'private/slice157-vu1-all-gen/manifest.json'
        if not path.exists():
            self.skipTest('private slice157 table not present')
        manifest = json.loads(path.read_text(encoding='utf-8'))
        self.assertEqual(manifest['entry_policy'], 'all')
        self.assertEqual(manifest['programs_per_file'], 16)
        self.assertEqual(sorted(p['entry'] for p in manifest['programs']), image_tool.all_entries())
        self.assertEqual(len(manifest['generated_parts']), 128)
        self.assertEqual(sum(part['programs'] for part in manifest['generated_parts']), 2048)
        self.assertTrue(all(p['status'] == 'emitted' and p['image_flags_used'] for p in manifest['programs']))


if __name__ == '__main__':
    unittest.main()
