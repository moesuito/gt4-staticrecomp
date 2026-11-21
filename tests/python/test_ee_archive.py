"""Independent synthetic archive controls, no proprietary object code."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from ee_archive import elf_object_inventory, object_inventory, inventory_digest


def archive(timestamp=1, machine=0x8664):
    body = struct.pack('<HHIIIHH', machine, 1, timestamp, 0, 0, 0, 0) + bytes(40)
    header = (b'table.obj/      '.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6)
              + b'0'.ljust(6) + b'0'.ljust(8) + str(len(body)).encode().ljust(10) + b'`\n')
    return b'!<arch>\n' + header + body


class ArchiveTests(unittest.TestCase):
    def test_long_member_names_and_invalid_offsets(self):
        def member(name, body):
            header = (name.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6)
                      + b'0'.ljust(8) + str(len(body)).encode().ljust(10) + b'`\n')
            return header + body + (b'\n' if len(body) & 1 else b'')
        first, second = b'folder\\table.obj\0', b'another\\unit_0000.obj\0'
        body = archive()[68:]
        data = (b'!<arch>\n' + member(b'//', first + second)
                + member(b'/0', body) + member(('/' + str(len(first))).encode(), body))
        rows = object_inventory(data)
        self.assertEqual([row['name'] for row in rows], ['table.obj', 'unit_0000.obj'])
        for names, reference in ((b'unterminated', b'/0'), (first, b'/999')):
            with self.subTest(reference=reference), self.assertRaises(ValueError):
                object_inventory(b'!<arch>\n' + member(b'//', names) + member(reference, body))

    def test_only_timestamp_is_normalized(self):
        first = object_inventory(archive(1))
        self.assertEqual(first, object_inventory(archive(2)))
        changed = bytearray(archive())
        changed[-1] = 1
        self.assertNotEqual(inventory_digest(first), inventory_digest(object_inventory(bytes(changed))))

    def test_invalid_and_non_amd64_archives_fail(self):
        for data in (b'', b'arbitrary bytes', b'!<arch>\n', archive()[:-1], archive(machine=0x14c)):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                object_inventory(data)


def elf_object(machine=62, object_type=1, elf_class=2, payload=b''):
    # Independent ELF64 header bytes (TASK-074 L1): magic, class, little-endian data, version, then e_type/e_machine.
    header = b'\x7fELF' + bytes([elf_class, 1, 1]) + bytes(9) + struct.pack('<HH', object_type, machine)
    return header + bytes(64 - len(header)) + payload


def gnu_member(name, body):
    header = (name.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6)
              + b'644'.ljust(8) + str(len(body)).encode().ljust(10) + b'`\n')
    return header + body + (b'\n' if len(body) & 1 else b'')


class ElfArchiveTests(unittest.TestCase):
    def test_short_and_long_member_names(self):
        long_names = b'unit_0000.cpp.o/\nunit_0001.cpp.o/\n'
        data = (b'!<arch>\n' + gnu_member(b'/', b'\0' * 4) + gnu_member(b'//', long_names)
                + gnu_member(b'table.cpp.o/', elf_object(payload=b'a'))
                + gnu_member(b'/0', elf_object(payload=b'b'))
                + gnu_member(b'/17', elf_object(payload=b'c')))
        rows = elf_object_inventory(data)
        self.assertEqual([row['name'] for row in rows], ['table.cpp.o', 'unit_0000.cpp.o', 'unit_0001.cpp.o'])
        self.assertEqual(len({row['sha256'] for row in rows}), 3)

    def test_member_bodies_are_hashed_and_headers_are_not(self):
        body = elf_object(payload=b'code')
        first = elf_object_inventory(b'!<arch>\n' + gnu_member(b'table.cpp.o/', body))
        later_date = gnu_member(b'table.cpp.o/', body).replace(b'0'.ljust(12), b'1700000000'.ljust(12), 1)
        self.assertEqual(first, elf_object_inventory(b'!<arch>\n' + later_date))
        changed = elf_object_inventory(b'!<arch>\n' + gnu_member(b'table.cpp.o/', elf_object(payload=b'cod3')))
        self.assertNotEqual(inventory_digest(first), inventory_digest(changed))

    def test_non_x86_64_relocatable_objects_and_bad_archives_fail(self):
        for body in (elf_object(machine=183), elf_object(object_type=2), elf_object(elf_class=1),
                     b'not an elf object'.ljust(64, b'\0'), elf_object()[:63]):
            with self.subTest(body=body[:20]), self.assertRaises(ValueError):
                elf_object_inventory(b'!<arch>\n' + gnu_member(b'table.cpp.o/', body))
        duplicate = gnu_member(b'table.cpp.o/', elf_object())
        for data in (b'', b'!<arch>\n', b'!<arch>\n' + duplicate + duplicate,
                     b'!<arch>\n' + gnu_member(b'/', b'\0' * 4) + gnu_member(b'/5', elf_object())):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                elf_object_inventory(data)


if __name__ == '__main__':
    unittest.main()
