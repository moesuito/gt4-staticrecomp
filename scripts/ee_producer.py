"""Prepare and regenerate pinned GT4 EE code without changing frozen producers."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import stat
import subprocess
import sys
import zlib

from gt4disc import fingerprint, inspect_disc, validate_manifest, differences
from ee_archive import elf_object_inventory, object_inventory, inventory_digest


ROOT = Path(__file__).resolve().parents[1]
DONOR_COMMIT = '1b4808793a12c45125262fd2d5f50a9ec9f7c020'
MAX_INFLATED_CORE = 8 * 1024 * 1024
R5900_FLAGS = 0x20924001
QUALIFIED_COMPILER = Path('C:/Antigravity/gt4-staticrecomp/private/tooling/llvm-23.1.2/bin/clang-cl.exe')
QUALIFIED_OBJECT_INVENTORY = '8800e261b025f160c56604f06b063573447cd949a3491db58d99942d6b4254aa'
# TASK-074 L1: the Linux candidate recipe. It is checked separately from the qualified Windows recipe above and never
# produces a production library: no Linux compiler or object inventory is pinned yet, so a Linux library is recorded
# as a candidate only (linux-library.json).
LINUX_COMPILER_IDS = ('Clang', 'GNU')
LINUX_RECIPE = {'cxx_standard': '20', 'build_type': 'Release', 'release_flags': '-O3 -DNDEBUG', 'flags': ''}
MANIFEST_PINS = {
    'usa-v2.00.json': '33f10ef008ca800067189fc40624d42b5c6027e3e273418e48d7b2b9c0587a92',
    'usa-v2.00-native.json': 'cca879092cbb6af89823706bc0a36e5ee498581a1bd9c79292e64e35f5d8f339',
    'external-recomp-references.json': 'f38617a18591b070257d5c707fa69f238a78fc5b635c46b9f5b7c2b6bd65753a',
}


def identity(path):
    with path.open('rb') as source:
        return fingerprint(source)


def verify_identity(path, expected):
    actual = identity(path)
    if actual != expected:
        raise ValueError(f'Pinned size/hash mismatch: {path}')
    return actual


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path, value):
    with path.open('x', encoding='utf-8') as destination:
        destination.write(json.dumps(value, indent=2, sort_keys=True) + '\n')


def trusted_manifests():
    result = {}
    for name, expected in MANIFEST_PINS.items():
        value = read_json(ROOT / 'docs/inputs' / name)
        canonical = json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
        if hashlib.sha256(canonical).hexdigest() != expected:
            raise ValueError(f'Input manifest changed from the reviewed baseline: {name}')
        result[name] = value
    return result


def reconstruct_elf(core, policy):
    """Reproduce the qualified ELF layout; its BSS policy is not PS2 fidelity proof."""
    if len(core) < 6:
        raise ValueError('Truncated CORE header')
    _, inflated_size = struct.unpack_from('<HI', core)
    if inflated_size > MAX_INFLATED_CORE:
        raise ValueError('CORE inflated size exceeds the qualified limit')
    decoder = zlib.decompressobj(-15)
    try:
        payload = decoder.decompress(core[6:], MAX_INFLATED_CORE + 1)
    except zlib.error as error:
        raise ValueError('Invalid CORE deflate stream') from error
    if (len(payload) != inflated_size or not decoder.eof or decoder.unused_data
            or decoder.unconsumed_tail):
        raise ValueError('Invalid CORE deflate stream or inflated size')

    def require(offset, size):
        if offset < 0 or size < 0 or offset + size > len(payload):
            raise ValueError('CORE record extends beyond the inflated payload')

    offset = 0
    for _ in range(2):
        require(offset, 2)
        length = struct.unpack_from('<H', payload, offset)[0]
        require(offset + 2, length)
        offset += 2 + length
    require(offset, 8)
    count, entry = struct.unpack_from('<iI', payload, offset)
    offset += 8
    def unsigned(value):
        return type(value) is int and 0 <= value <= 0xFFFFFFFF

    records = policy.get('records')
    if not isinstance(records, list) or not unsigned(policy.get('entry_address')):
        raise ValueError('Invalid qualified CORE policy')
    for record in records:
        if not isinstance(record, dict):
            raise ValueError('Invalid qualified CORE record')
        for key in ('core_target_address', 'size_bytes', 'inflated_payload_offset'):
            if not unsigned(record.get(key)):
                raise ValueError('Invalid qualified CORE record field')
        header = record.get('reference_program_header')
        if not isinstance(header, dict) or any(not unsigned(header.get(key)) for key in (
                'type', 'file_offset', 'virtual_address', 'physical_address',
                'file_size', 'memory_size', 'flags', 'alignment')):
            raise ValueError('Invalid qualified ELF program header')
    if count != len(records) or count <= 0 or count > 16 or entry != policy['entry_address']:
        raise ValueError('CORE entry/record count differs from qualified policy')
    parts = []
    for record in records:
        require(offset, 8)
        address, size = struct.unpack_from('<II', payload, offset)
        require(offset + 8, size)
        body = payload[offset + 8:offset + 8 + size]
        if (address != record['core_target_address'] or size != record['size_bytes']
                or offset + 8 != record['inflated_payload_offset']
                or hashlib.sha256(body).hexdigest() != record['payload_sha256']):
            raise ValueError('CORE record differs from qualified policy')
        parts.append((record['reference_program_header'], body))
        offset += 8 + size
    if offset != len(payload):
        raise ValueError('Unexpected trailing CORE payload')

    if type(policy.get('native_elf_size')) is not int or not 52 <= policy['native_elf_size'] <= 16 * 1024 * 1024:
        raise ValueError('ELF size exceeds the qualified limit')
    output = bytearray(policy['native_elf_size'])
    headers = sorted(parts, key=lambda part: part[0]['file_offset'])
    if 52 + 32 * count > len(output):
        raise ValueError('ELF header table exceeds output size')
    output[:16] = b'\x7fELF\x01\x01\x01' + b'\0' * 9
    struct.pack_into('<HHIIIIIHHHHHH', output, 16, 2, 8, 1, entry,
                     52, 0, R5900_FLAGS, 52, 32, count, 0, 0, 0)
    ranges = [(0, 52 + 32 * count)]
    for index, (header, body) in enumerate(headers):
        start, size = header['file_offset'], header['file_size']
        end = start + size
        if (size != len(body) or end > len(output) or header['memory_size'] < size
                or header['virtual_address'] + header['memory_size'] > 0x100000000
                or header['physical_address'] + header['memory_size'] > 0x100000000
                or any(start < previous_end and end > previous_start
                       for previous_start, previous_end in ranges)):
            raise ValueError('Invalid or overlapping ELF file layout')
        struct.pack_into('<8I', output, 52 + index * 32, header['type'], start,
                         header['virtual_address'], header['physical_address'], size,
                         header['memory_size'], header['flags'], header['alignment'])
        output[start:end] = body
        ranges.append((start, end))
    result = bytes(output)
    if hashlib.sha256(result).hexdigest() != policy['native_elf_sha256']:
        raise ValueError('Reconstructed ELF differs from the pinned native ELF')
    return result


def reject_link(path):
    if path.is_symlink() or (path.exists() and
            getattr(path.lstat(), 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
        raise ValueError(f'Producer artifact must not be a symbolic link or reparse point: {path}')


def producer_path(output):
    """Producer outputs are immediate children of an assets checkout's private/ (the checkout that holds the
    verified donor and the protected evidence). Worktrees pass the main checkout as the assets root."""
    reject_link(output.parent)
    reject_link(output)
    output = output.resolve()
    if output.parent.name != 'private':
        raise ValueError('Producer must be an immediate child of an assets checkout private/')
    if output.name.lower() in ('references', 'manual-play', 'reference-acceleration-study'):
        raise ValueError('Producer output is a protected asset/evidence directory')
    return output


def validate_output(output, assets_root):
    output = producer_path(output)
    if output.parent != (assets_root / 'private').resolve():
        raise ValueError('Producer output must sit in the assets checkout private/ it was prepared from')
    for protected in (assets_root / 'private/references', assets_root / 'private/manual-play',
                      assets_root / 'private/reference-acceleration-study'):
        protected = protected.resolve()
        if output.is_relative_to(protected) or protected.is_relative_to(output):
            raise ValueError('Producer output overlaps protected assets or evidence')
    if output.exists():
        raise ValueError('Producer output already exists; choose a fresh identity')
    return output


def verify_donor(assets_root, reference):
    donor = assets_root / 'private/references/ps2-web-recomp'
    commit = subprocess.check_output(['git', '-C', str(donor), 'rev-parse', 'HEAD'],
                                     text=True).strip()
    status = subprocess.check_output(['git', '-C', str(donor), 'status', '--porcelain'],
                                     text=True).strip()
    if commit != DONOR_COMMIT or status:
        raise ValueError('Donor revision/worktree differs from the frozen source')
    for pin in reference['source_pins']:
        verify_identity(assets_root / pin['path'],
                        {key: pin[key] for key in ('size_bytes', 'sha256')})
    return donor


def generated_inventory(folder):
    reject_link(folder)
    if any(not path.is_file() or path.is_symlink() for path in folder.iterdir()):
        raise ValueError('Unexpected generated subdirectory or symbolic link')
    files = sorted(path for path in folder.iterdir() if path.is_file())
    records = []
    for path in files:
        if not (path.name == 'units.hpp' or path.name == 'table.cpp'
                or (path.name.startswith('unit_') and path.suffix == '.cpp')):
            raise ValueError(f'Unexpected generated artifact: {path.name}')
        records.append({'path': path.name, **identity(path)})
    return records


def inventory_summary(records):
    canonical = [{'path': 'private/reference-acceleration-study/generated/' + row['path'],
                  'size_bytes': row['size_bytes'], 'sha256': row['sha256']} for row in records]
    return {'file_count': len(records), 'size_bytes': sum(row['size_bytes'] for row in records),
            'inventory_sha256': hashlib.sha256(json.dumps(canonical, sort_keys=True).encode()).hexdigest()}


def existing_producer(output):
    output = producer_path(output)
    for name in ('producer.json', 'CORE.GT4', 'SCUS_973.28.elf'):
        reject_link(output / name)
    metadata = read_json(output / 'producer.json')
    if Path(metadata['assets_root']).resolve() / 'private' != output.parent:
        raise ValueError('Producer assets root differs from the checkout that holds this producer')
    manifests = trusted_manifests()
    for name, pin in metadata['input_manifests'].items():
        verify_identity(ROOT / 'docs/inputs' / name, pin)
    study = manifests['external-recomp-references.json']
    reference = next(item for item in study['references'] if item['name'] == 'ps2-web-recomp')
    donor = verify_donor(Path(metadata['assets_root']), reference)
    if Path(metadata['donor']).resolve() != donor.resolve() or metadata['donor_commit'] != DONOR_COMMIT:
        raise ValueError('Producer donor differs from the reviewed source')
    if metadata['generation_options'] != {'entries': None, 'modules': None, 'probes': None}:
        raise ValueError('Producer options differ from the baseline generation recipe')
    policy = manifests['usa-v2.00-native.json']
    elf_pin = {'size_bytes': policy['native_elf_size'], 'sha256': policy['native_elf_sha256']}
    core_pin = manifests['usa-v2.00.json']['files']['/CORE.GT4;1']
    if metadata['elf'] != elf_pin or metadata['core'] != core_pin:
        raise ValueError('Producer receipt differs from reviewed input pins')
    verify_identity(output / 'SCUS_973.28.elf', elf_pin)
    verify_identity(output / 'CORE.GT4', core_pin)
    return output, metadata, study


def verify_toolchain(path, metadata, generated=None):
    reject_link(path.parent)
    reject_link(path)
    fields = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        key, separator, value = line.partition('=')
        if not separator or key in fields:
            raise ValueError('Malformed compiler configuration receipt')
        fields[key] = value
    if (fields.get('compiler_id') != 'Clang' or fields.get('compiler_version') != '23.1.2'
            or fields.get('cxx_standard') != '20' or fields.get('build_type') != 'Release'
            or fields.get('release_flags') != '/O2 /Ob2 /DNDEBUG'
            or fields.get('flags') != '/DWIN32 /D_WINDOWS /EHsc'
            or Path(fields.get('compiler', '')).resolve() != QUALIFIED_COMPILER.resolve()
            or Path(fields.get('donor', '')).resolve() != Path(metadata['donor']).resolve()):
        raise ValueError('Compiler/source configuration differs from the qualified recipe')
    if generated is not None and Path(fields.get('generated', '')).resolve() != generated:
        raise ValueError('Compiler generated-source directory differs from producer')
    return identity(path)


def on_linux():
    return sys.platform.startswith('linux')


def translator_file_name():
    return 'eerecomp' if on_linux() else 'eerecomp.exe'


def toolchain_fields(path):
    reject_link(path.parent)
    reject_link(path)
    fields = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        key, separator, value = line.partition('=')
        if not separator or key in fields:
            raise ValueError('Malformed compiler configuration receipt')
        fields[key] = value
    return fields


def verify_linux_toolchain(path, metadata, generated=None):
    """TASK-074 L1: the Linux candidate recipe (clang++ or g++, C++20, CMake Release flags, no extra flags).
    The compiler version is recorded, not pinned: there is no qualified Linux compiler yet."""
    fields = toolchain_fields(path)
    compiler = Path(fields.get('compiler', ''))
    if (fields.get('compiler_id') not in LINUX_COMPILER_IDS or not fields.get('compiler_version')
            or any(fields.get(key) != value for key, value in LINUX_RECIPE.items())
            or not compiler.is_absolute() or not compiler.is_file()
            or Path(fields.get('donor', '')).resolve() != Path(metadata['donor']).resolve()):
        raise ValueError('Compiler/source configuration differs from the Linux candidate recipe')
    if generated is not None and Path(fields.get('generated', '')).resolve() != generated:
        raise ValueError('Compiler generated-source directory differs from producer')
    return identity(path)


def verify_host_toolchain(path, metadata, generated=None):
    if on_linux():
        return verify_linux_toolchain(path, metadata, generated)
    return verify_toolchain(path, metadata, generated)


def prepare(assets_root, output, iso=None, core=None):
    assets_root = assets_root.resolve()
    output = validate_output(output, assets_root)
    manifests = trusted_manifests()
    study = manifests['external-recomp-references.json']
    disc = manifests['usa-v2.00.json']
    policy = manifests['usa-v2.00-native.json']
    validate_manifest(disc)
    reference = next(item for item in study['references'] if item['name'] == 'ps2-web-recomp')
    donor = verify_donor(assets_root, reference)
    if iso is not None:
        iso = iso.resolve()
        observed = inspect_disc(iso)
        if differences(disc, observed):
            raise ValueError('ISO identity differs from pinned USA v2.00')
        import pycdlib
        image = pycdlib.PyCdlib()
        image.open(str(iso))
        try:
            with image.open_file_from_iso(iso_path='/CORE.GT4;1') as source:
                core_bytes = source.read(disc['files']['/CORE.GT4;1']['size_bytes'] + 1)
        finally:
            image.close()
        core_source = str(iso)
    else:
        core = (core or assets_root / 'private/fingerprint-check/CORE.GT4').resolve()
        verify_identity(core, disc['files']['/CORE.GT4;1'])
        core_bytes = core.read_bytes()
        core_source = str(core)
    core_identity = {'size_bytes': len(core_bytes), 'sha256': hashlib.sha256(core_bytes).hexdigest()}
    if core_identity != disc['files']['/CORE.GT4;1']:
        raise ValueError('Extracted CORE differs from pinned USA v2.00')
    elf = reconstruct_elf(core_bytes, policy)
    output.mkdir(parents=True)
    (output / 'CORE.GT4').write_bytes(core_bytes)
    (output / 'SCUS_973.28.elf').write_bytes(elf)
    metadata = {'schema_version': 1, 'assets_root': str(assets_root),
                'donor': str(donor), 'donor_commit': DONOR_COMMIT,
                'core_source': core_source, 'core': core_identity,
                'elf': identity(output / 'SCUS_973.28.elf'),
                'generation_options': {'entries': None, 'modules': None, 'probes': None},
                'input_manifests': {name: identity(ROOT / 'docs/inputs' / name)
                                    for name in ('usa-v2.00.json', 'usa-v2.00-native.json',
                                                 'external-recomp-references.json')},
                'qualification': 'Prepared only; no runtime/gameplay qualification'}
    write_json(output / 'producer.json', metadata)
    return metadata


def generate(output, translator=None):
    output, metadata, study = existing_producer(output)
    assets_root = Path(metadata['assets_root'])
    if (output / 'generated').exists() or (output / 'generation.json').exists():
        raise ValueError('Generation already attempted; preserve producer and choose a fresh identity')
    if translator is None:
        pin = next(item for item in study['artifacts'] if item['path'].endswith('/eerecomp.exe'))
        translator = assets_root / pin['path']
        verify_identity(translator, {key: pin[key] for key in ('size_bytes', 'sha256')})
        tool_origin = 'Frozen pinned translator; no compiler rebuild in this stage'
        toolchain = None
    else:
        reject_link(translator.parent)
        reject_link(translator)
        translator = translator.resolve()
        if translator != output / 'tool-build' / translator_file_name():
            raise ValueError('New translator must be built in this producer tool-build/')
        toolchain = verify_host_toolchain(output / 'tool-build/toolchain.txt', metadata)
        tool_origin = 'Fresh producer translator; compiler receipt in tool-build/toolchain.txt'
    arguments = [str(translator), str(output / 'SCUS_973.28.elf'),
                 '--out', str(output / 'generated')]
    with (output / 'generation.log').open('xb') as log:
        subprocess.run(arguments, stdout=log, stderr=subprocess.STDOUT, check=True)
    records = generated_inventory(output / 'generated')
    expected = study['generated']
    observed = inventory_summary(records)
    result = {'schema_version': 1, 'translator': identity(translator), 'tool_origin': tool_origin,
              'toolchain': toolchain, 'arguments': arguments, 'generated': records, 'comparison': observed,
              'identical_to_frozen_generated': observed == expected,
              'qualification': 'Source identity only; not runtime or PS2 fidelity proof'}
    if on_linux():
        result['host'] = 'linux-candidate'
    write_json(output / 'generation.json', result)
    if observed != expected:
        raise ValueError('Generated source differs from frozen producer; see private generation.json')
    return result


def record_library(output):
    output, metadata, study = existing_producer(output)
    generation = read_json(output / 'generation.json')
    records = generated_inventory(output / 'generated')
    if records != generation['generated']:
        raise ValueError('Generated source changed after verification')
    if inventory_summary(records) != study['generated']:
        raise ValueError('Library sources differ from pinned generated inventory')
    if on_linux():
        return record_linux_library(output, metadata, records)
    library = output / 'game-build/ps2game.lib'
    compiler_receipt = output / 'game-build/toolchain.txt'
    reject_link(library.parent)
    reject_link(library)
    receipt_identity = verify_toolchain(compiler_receipt, metadata, output / 'generated')
    objects = object_inventory(library.read_bytes())
    expected_members = {row['path'] + '.obj' for row in records if row['path'].endswith('.cpp')}
    if {row['name'] for row in objects} != expected_members:
        raise ValueError('Library member inventory differs from generated compilation units')
    object_digest = inventory_digest(objects)
    if object_digest != QUALIFIED_OBJECT_INVENTORY:
        raise ValueError('COFF objects differ from the qualified baseline after timestamp normalization')
    result = {'schema_version': 1, 'library': identity(library),
              'compiler_receipt': receipt_identity,
              'object_count': len(objects), 'object_inventory_without_timestamps': object_digest,
              'qualification': 'Library produced; integration and owner gameplay pending'}
    write_json(output / 'library.json', result)
    return result


def record_linux_library(output, metadata, records):
    """TASK-074 L1: record a Linux candidate library built from the verified generated sources. Nothing here is a
    qualified pin; the Windows production library and launcher pins are untouched."""
    library = output / 'game-build/libps2game.a'
    reject_link(library.parent)
    reject_link(library)
    receipt_identity = verify_linux_toolchain(output / 'game-build/toolchain.txt', metadata, output / 'generated')
    objects = elf_object_inventory(library.read_bytes())
    expected_members = {row['path'] + '.o' for row in records if row['path'].endswith('.cpp')}
    if {row['name'] for row in objects} != expected_members:
        raise ValueError('Library member inventory differs from generated compilation units')
    result = {'schema_version': 1, 'host': 'linux-candidate', 'library': identity(library),
              'compiler_receipt': receipt_identity,
              'object_count': len(objects), 'object_inventory': inventory_digest(objects),
              'qualification': 'Linux candidate library from verified sources; unpinned, not production, '
                               'integration and owner gameplay pending'}
    write_json(output / 'linux-library.json', result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prep = commands.add_parser('prepare', help='Verify inputs and reconstruct ELF in a fresh private directory')
    prep.add_argument('--assets-root', type=Path, required=True)
    prep.add_argument('--output', type=Path, required=True)
    inputs = prep.add_mutually_exclusive_group()
    inputs.add_argument('--iso', type=Path)
    inputs.add_argument('--core', type=Path)
    gen = commands.add_parser('generate', help='Generate source and compare every file with frozen provenance')
    gen.add_argument('--output', type=Path, required=True)
    gen.add_argument('--translator', type=Path)
    lib = commands.add_parser('record-library', help='Record an independently built library without repinning production')
    lib.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == 'prepare':
            result = prepare(args.assets_root, args.output, args.iso, args.core)
            print('Prepared pinned CORE/ELF and frozen donor recipe in a fresh private producer.')
        elif args.command == 'generate':
            result = generate(args.output, args.translator)
            print(f"Verified {result['comparison']['file_count']} generated files against frozen provenance.")
        else:
            result = record_library(args.output)
            print('Recorded private AOT library; production pins and launcher unchanged.')
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError) as error:
        print(f'ee-producer: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
