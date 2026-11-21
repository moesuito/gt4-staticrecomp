"""Bounded COFF archive metadata inspection; no guest execution."""

import hashlib
import json
import struct


def object_inventory(data):
    if data[:8] != b'!<arch>\n':
        raise ValueError('Expected COFF library archive')
    offset, names, objects = 8, b'', []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid COFF archive member header')
        size_text = header[48:58].strip()
        if not size_text.isdigit():
            raise ValueError('Invalid COFF archive member size')
        size = int(size_text)
        start, end = offset + 60, offset + 60 + size
        if end + (size & 1) > len(data):
            raise ValueError('Truncated COFF archive member')
        member = header[:16].rstrip()
        body = data[start:end]
        if member == b'//':
            names = body
        elif member != b'/':
            if member.startswith(b'/') and member[1:].isdigit():
                index = int(member[1:])
                terminator = names.find(b'\0', index)
                if index >= len(names) or terminator < 0:
                    raise ValueError('Invalid COFF long member name')
                name = names[index:terminator].decode('utf-8')
            else:
                name = member.rstrip(b'/').decode('utf-8')
            name = name.replace('\\', '/').rsplit('/', 1)[-1]
            if len(body) < 20:
                raise ValueError('Truncated COFF object')
            machine, sections, _, symbols, count, optional_size, _ = struct.unpack_from('<HHIIIHH', body)
            if (machine != 0x8664 or sections == 0 or 20 + optional_size + 40 * sections > len(body)
                    or symbols + 18 * count > len(body)):
                raise ValueError('Expected bounded AMD64 COFF object')
            normalized = body[:4] + b'\0' * 4 + body[8:]
            objects.append({'name': name, 'size_bytes': len(body),
                            'sha256_without_timestamp': hashlib.sha256(normalized).hexdigest()})
        offset = end + (size & 1)
    if not objects or len({row['name'] for row in objects}) != len(objects):
        raise ValueError('Empty or duplicate COFF object inventory')
    return sorted(objects, key=lambda row: row['name'])


def elf_object_inventory(data):
    """TASK-074 L1: the Linux counterpart of object_inventory for a GNU ar archive of x86-64 ELF relocatable objects.
    ELF objects carry no timestamp, so each member body is hashed as it is; the archive's own member headers (dates,
    owners) are not part of the inventory."""
    if data[:8] != b'!<arch>\n':
        raise ValueError('Expected ar library archive')
    offset, names, objects = 8, b'', []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid ar archive member header')
        size_text = header[48:58].strip()
        if not size_text.isdigit():
            raise ValueError('Invalid ar archive member size')
        size = int(size_text)
        start, end = offset + 60, offset + 60 + size
        if end + (size & 1) > len(data):
            raise ValueError('Truncated ar archive member')
        member = header[:16].rstrip()
        body = data[start:end]
        if member == b'//':
            names = body
        elif member not in (b'/', b'/SYM64/'):
            if member.startswith(b'/') and member[1:].isdigit():
                index = int(member[1:])
                terminator = names.find(b'/\n', index)
                if index >= len(names) or terminator < 0:
                    raise ValueError('Invalid ar long member name')
                name = names[index:terminator].decode('utf-8')
            elif member.endswith(b'/'):
                name = member[:-1].decode('utf-8')
            else:
                raise ValueError('Unexpected ar member name form')
            name = name.rsplit('/', 1)[-1]
            if len(body) < 64:
                raise ValueError('Truncated ELF object')
            elf_class, encoding = body[4], body[5]
            object_type, machine = struct.unpack_from('<HH', body, 16)
            if body[:4] != b'\x7fELF' or elf_class != 2 or encoding != 1 or object_type != 1 or machine != 62:
                raise ValueError('Expected x86-64 ELF relocatable object')
            objects.append({'name': name, 'size_bytes': len(body), 'sha256': hashlib.sha256(body).hexdigest()})
        offset = end + (size & 1)
    if not objects or len({row['name'] for row in objects}) != len(objects):
        raise ValueError('Empty or duplicate ELF object inventory')
    return sorted(objects, key=lambda row: row['name'])


def inventory_digest(objects):
    return hashlib.sha256(json.dumps(objects, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
