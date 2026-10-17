"""Synthetic parser controls and shape checks on the committed name table; no game data."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import ee_symbols as symbols

TEXT_START, TEXT_END = 0x00100000, 0x00617A14


class ParserControls(unittest.TestCase):
    def test_symbol_lines(self):
        rows = symbols.parse_symbol_addrs(
            '// comment\n\nCameraBase__tf = 0x00603D80; // type:func\n'
            'RaceBase__updateRace = 0x00389B08; // type:func gthd\n'
            'X__vtable = 0x00688450; // type:data\n'
            'CameraBase__virtual_00 = 0x0044F198; // type:func\n')
        self.assertEqual([(r['address'], r['kind'], r['origin'], r['quality']) for r in rows], [
            ('0x00603D80', 'func', 'decomp', 'named'), ('0x00389B08', 'func', 'gthd', 'named'),
            ('0x00688450', 'data', 'decomp', 'named'), ('0x0044F198', 'func', 'decomp', 'placeholder')])

    def test_bad_lines_are_rejected(self):
        with self.assertRaises(ValueError):
            symbols.parse_symbol_addrs('not a symbol\n')
        with self.assertRaises(ValueError):
            symbols.parse_adhoc_methods('Class method\n')

    def test_adhoc_and_merge(self):
        adhoc = symbols.parse_adhoc_methods('# header\nMQuickWork get_SessionNumber 0x005C3EB0\n')
        self.assertEqual(adhoc[0]['name'], 'MQuickWork::get_SessionNumber')
        table = symbols.build_table(adhoc + adhoc, adhoc)
        self.assertEqual(len(table), 1)


class CommittedTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = symbols.read_csv(ROOT / 'docs' / 'data' / 'ee-names.csv')

    def test_columns_and_order(self):
        self.assertTrue(self.rows)
        self.assertEqual(list(self.rows[0]), symbols.COLUMNS)
        keys = [(r['address'], r['name']) for r in self.rows]
        self.assertEqual(keys, sorted(set(keys)))

    def test_function_addresses_are_aligned_code(self):
        for row in self.rows:
            address = int(row['address'], 16)
            self.assertRegex(row['address'], r'^0x[0-9A-F]{8}$')
            if row['kind'] == 'func':
                self.assertEqual(address % 4, 0, row)
                self.assertTrue(TEXT_START <= address < TEXT_END, row)
            else:
                self.assertGreaterEqual(address, TEXT_END, row)


if __name__ == '__main__':
    unittest.main()
