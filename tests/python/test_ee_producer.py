"""Independent synthetic controls for the private EE producer, without game data."""

import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import ee_producer


def synthetic_core():
    body = b'fixture payload'
    prefix = b'\x01\x00a\x01\x00b' + struct.pack('<iI', 1, 0x100008)
    payload = prefix + struct.pack('<II', 0x100000, len(body)) + body
    compressor = zlib.compressobj(wbits=-15)
    core = struct.pack('<HI', 0, len(payload)) + compressor.compress(payload) + compressor.flush()
    # Independent ELF bytes and program header, not produced by the implementation.
    header = (b'\x7fELF\x01\x01\x01' + b'\0' * 9
              + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, 0x100008,
                            52, 0, 0x20924001, 52, 32, 1, 0, 0, 0)
              + struct.pack('<8I', 1, 512, 0x100000, 0x100000,
                            len(body), len(body) + 8, 7, 4096))
    expected = header + b'\0' * (512 - len(header)) + body
    policy = {'entry_address': 0x100008, 'native_elf_size': len(expected),
              'native_elf_sha256': hashlib.sha256(expected).hexdigest(),
              'records': [{'core_target_address': 0x100000, 'size_bytes': len(body),
                           'inflated_payload_offset': len(prefix) + 8,
                           'payload_sha256': hashlib.sha256(body).hexdigest(),
                           'reference_program_header': {
                               'type': 1, 'file_offset': 512, 'virtual_address': 0x100000,
                               'physical_address': 0x100000, 'file_size': len(body),
                               'memory_size': len(body) + 8, 'flags': 7, 'alignment': 4096}}]}
    return core, policy, expected


class ReconstructionTests(unittest.TestCase):
    def test_independent_elf_and_bss_policy(self):
        core, policy, expected = synthetic_core()
        self.assertEqual(ee_producer.reconstruct_elf(core, policy), expected)

    def test_invalid_streams_and_resource_limits(self):
        core, policy, _ = synthetic_core()
        invalid = [b'', core[:5], core[:-1], core + b'trailing',
                   core[:2] + struct.pack('<I', 1) + core[6:],
                   core[:2] + struct.pack('<I', 0xffffffff) + core[6:]]
        for data in invalid:
            with self.subTest(data=data[:10]), self.assertRaises(ValueError):
                ee_producer.reconstruct_elf(data, policy)

    def test_wrong_records_and_unsafe_layouts(self):
        core, original, _ = synthetic_core()
        mutations = [({'entry_address': 3}, None), ({'native_elf_size': 0xffffffff}, None),
                     ({'native_elf_size': True}, None), ({'native_elf_sha256': '0' * 64}, None),
                     ({'core_target_address': 1}, 'record'),
                     ({'payload_sha256': '0' * 64}, 'record'),
                     ({'size_bytes': 1000}, 'record'),
                     ({'file_offset': 64}, 'header'),
                     ({'memory_size': 1}, 'header'),
                     ({'virtual_address': 0xffffffff}, 'header'),
                     ({'file_offset': -1}, 'header'), ({'file_offset': True}, 'header')]
        for values, target in mutations:
            policy = copy.deepcopy(original)
            destination = (policy['records'][0]['reference_program_header'] if target == 'header'
                           else policy['records'][0] if target == 'record' else policy)
            destination.update(values)
            with self.subTest(values=values), self.assertRaises(ValueError):
                ee_producer.reconstruct_elf(core, policy)


class ProducerControls(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.addCleanup(patch.stopall)
        patch.object(ee_producer, 'ROOT', self.root).start()

    def test_fresh_output_and_protected_paths(self):
        valid = self.root / 'private/slice-test'
        self.assertEqual(ee_producer.validate_output(valid, self.root), valid)
        for path in (self.root / 'docs/out', valid / 'nested',
                     self.root / 'private/references', self.root / 'private/manual-play',
                     self.root / 'private/reference-acceleration-study'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                ee_producer.validate_output(path, self.root)
        valid.mkdir(parents=True)
        with self.assertRaises(ValueError):
            ee_producer.validate_output(valid, self.root)

    def test_changed_manifest_cannot_repin_itself(self):
        directory = self.root / 'docs/inputs'
        directory.mkdir(parents=True)
        path = directory / 'test.json'
        value = {'pin': 'reviewed'}
        digest = hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        with patch.object(ee_producer, 'MANIFEST_PINS', {'test.json': digest}):
            path.write_text(json.dumps(value, indent=4), encoding='utf-8')
            self.assertEqual(ee_producer.trusted_manifests()['test.json'], value)
            path.write_text('{"pin":"unreviewed"}', encoding='utf-8')
            with self.assertRaises(ValueError):
                ee_producer.trusted_manifests()

    def test_existing_stages_reject_protected_roots_before_receipts(self):
        for name in ('references', 'manual-play', 'reference-acceleration-study'):
            output = self.root / 'private' / name
            output.mkdir(parents=True)
            (output / 'producer.json').write_text('{}', encoding='utf-8')
            for stage in (ee_producer.existing_producer, ee_producer.generate, ee_producer.record_library):
                with self.subTest(name=name, stage=stage), self.assertRaisesRegex(ValueError, 'protected'):
                    stage(output)
            self.assertEqual([path.name for path in output.iterdir()], ['producer.json'])

    def test_reparse_points_rejected_without_reading_target(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        path = Mock(spec=Path)
        path.is_symlink.return_value = False
        path.exists.return_value = True
        path.lstat.return_value = SimpleNamespace(st_file_attributes=0x400)
        with self.assertRaisesRegex(ValueError, 'reparse'):
            ee_producer.reject_link(path)
        path.is_symlink.return_value = True
        with self.assertRaises(ValueError):
            ee_producer.generated_inventory(path)

    def test_inventory_detects_modified_source_and_extra_artifacts(self):
        directory = self.root / 'generated'
        directory.mkdir()
        source = directory / 'unit_0000.cpp'
        source.write_bytes(b'abc')
        first = ee_producer.generated_inventory(directory)
        self.assertEqual(first[0]['sha256'], 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')
        source.write_bytes(b'abd')
        self.assertNotEqual(ee_producer.inventory_summary(first),
                            ee_producer.inventory_summary(ee_producer.generated_inventory(directory)))
        (directory / 'nested').mkdir()
        with self.assertRaises(ValueError):
            ee_producer.generated_inventory(directory)

    def test_forged_generation_boolean_does_not_qualify_library(self):
        output = self.root / 'private/producer'
        generated = output / 'generated'
        generated.mkdir(parents=True)
        (generated / 'unit_0000.cpp').write_bytes(b'unreviewed source')
        records = ee_producer.generated_inventory(generated)
        (output / 'generation.json').write_text(json.dumps({
            'generated': records, 'identical_to_frozen_generated': True}), encoding='utf-8')
        with patch.object(ee_producer, 'existing_producer', return_value=(
                output, {}, {'generated': {'file_count': 328}})):
            with self.assertRaisesRegex(ValueError, 'pinned generated inventory'):
                ee_producer.record_library(output)
        self.assertFalse((output / 'library.json').exists())

    def test_forged_producer_input_receipt_cannot_replace_reviewed_pins(self):
        output = self.root / 'private/producer'
        output.mkdir(parents=True)
        donor = self.root / 'donor'
        pin = {'size_bytes': 4, 'sha256': 'a' * 64}
        manifests = {'external-recomp-references.json': {
            'references': [{'name': 'ps2-web-recomp'}]},
            'usa-v2.00-native.json': {'native_elf_size': 4, 'native_elf_sha256': 'a' * 64},
            'usa-v2.00.json': {'files': {'/CORE.GT4;1': pin}}}
        metadata = {'assets_root': str(self.root), 'input_manifests': {},
                    'donor': str(donor), 'donor_commit': ee_producer.DONOR_COMMIT,
                    'generation_options': {'entries': None, 'modules': None, 'probes': None},
                    'elf': {**pin, 'sha256': 'b' * 64}, 'core': pin}
        (output / 'producer.json').write_text(json.dumps(metadata), encoding='utf-8')
        with patch.object(ee_producer, 'trusted_manifests', return_value=manifests), \
                patch.object(ee_producer, 'verify_donor', return_value=donor), \
                patch.object(ee_producer, 'verify_identity') as verify:
            with self.assertRaisesRegex(ValueError, 'reviewed input pins'):
                ee_producer.existing_producer(output)
            verify.assert_not_called()

    def test_toolchain_requires_qualified_configuration(self):
        path = self.root / 'toolchain.txt'
        fields = {'compiler': str(ee_producer.QUALIFIED_COMPILER), 'compiler_id': 'Clang',
                  'compiler_version': '23.1.2', 'cxx_standard': '20', 'build_type': 'Release',
                  'flags': '/DWIN32 /D_WINDOWS /EHsc', 'release_flags': '/O2 /Ob2 /DNDEBUG',
                  'donor': str(self.root / 'donor'), 'generated': str(self.root / 'generated')}
        metadata = {'donor': fields['donor']}
        def write(values):
            path.write_text(''.join(f'{key}={value}\n' for key, value in values.items()), encoding='utf-8')
        write(fields)
        ee_producer.verify_toolchain(path, metadata, self.root / 'generated')
        for key, value in (('compiler_version', '99'), ('compiler', 'cl.exe'),
                           ('compiler', 'C:/fake/clang-cl.exe'),
                           ('flags', '/unsafe'), ('generated', str(self.root / 'other'))):
            changed = {**fields, key: value}
            write(changed)
            with self.subTest(key=key), self.assertRaises(ValueError):
                ee_producer.verify_toolchain(path, metadata, self.root / 'generated')
        path.write_text('compiler_id=Clang\ncompiler_id=Clang\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            ee_producer.verify_toolchain(path, metadata)

    def test_linux_toolchain_candidate_recipe(self):
        # TASK-074 L1: the Linux recipe is its own check; the qualified Windows recipe never accepts it.
        compiler = self.root / 'clang++'
        compiler.write_bytes(b'')
        path = self.root / 'toolchain.txt'
        fields = {'compiler': str(compiler), 'compiler_id': 'Clang', 'compiler_version': '23.1.4',
                  'cxx_standard': '20', 'build_type': 'Release', 'flags': '', 'release_flags': '-O3 -DNDEBUG',
                  'donor': str(self.root / 'donor'), 'generated': str(self.root / 'generated')}
        metadata = {'donor': fields['donor']}
        def write(values):
            path.write_text(''.join(f'{key}={value}\n' for key, value in values.items()), encoding='utf-8')
        write(fields)
        ee_producer.verify_linux_toolchain(path, metadata, self.root / 'generated')
        write({**fields, 'compiler_id': 'GNU'})
        ee_producer.verify_linux_toolchain(path, metadata, self.root / 'generated')
        write(fields)
        with self.assertRaises(ValueError):
            ee_producer.verify_toolchain(path, metadata, self.root / 'generated')
        for key, value in (('compiler_id', 'MSVC'), ('compiler_version', ''), ('compiler', 'clang++'),
                           ('compiler', str(self.root / 'missing')), ('flags', '-ffast-math'),
                           ('release_flags', '-O0'), ('cxx_standard', '17'), ('build_type', 'Debug'),
                           ('donor', str(self.root / 'other')), ('generated', str(self.root / 'other'))):
            write({**fields, key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                ee_producer.verify_linux_toolchain(path, metadata, self.root / 'generated')

    def test_host_selects_recipe_and_translator_name(self):
        with patch.object(ee_producer, 'on_linux', return_value=True), \
                patch.object(ee_producer, 'verify_linux_toolchain', return_value='linux') as linux, \
                patch.object(ee_producer, 'verify_toolchain', return_value='windows') as windows:
            self.assertEqual(ee_producer.translator_file_name(), 'eerecomp')
            self.assertEqual(ee_producer.verify_host_toolchain(self.root / 't', {}), 'linux')
            windows.assert_not_called()
        with patch.object(ee_producer, 'on_linux', return_value=False), \
                patch.object(ee_producer, 'verify_linux_toolchain', return_value='linux') as linux, \
                patch.object(ee_producer, 'verify_toolchain', return_value='windows'):
            self.assertEqual(ee_producer.translator_file_name(), 'eerecomp.exe')
            self.assertEqual(ee_producer.verify_host_toolchain(self.root / 't', {}), 'windows')
            linux.assert_not_called()

    def test_lexical_private_link_is_rejected_before_resolution(self):
        original = ee_producer.reject_link
        def reject(path):
            if path == self.root / 'private':
                raise ValueError('Synthetic private root reparse point')
            original(path)
        with patch.object(ee_producer, 'reject_link', side_effect=reject):
            for stage in (ee_producer.producer_path, ee_producer.existing_producer):
                with self.assertRaisesRegex(ValueError, 'private root reparse'):
                    stage(self.root / 'private/candidate')


if __name__ == '__main__':
    unittest.main()
