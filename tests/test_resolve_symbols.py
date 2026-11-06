#!/usr/bin/env python3
"""Run the real harvester with mocked PE/PDB inputs; no network or Windows files."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('resolve_symbols', ROOT / 'packaging/harvest/resolve_symbols.py')
harvester = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harvester)
TEARDOWN = '?TearDown@CoreApplication@Core@ApplicationModel@Windows@@QEAAJXZ'


class HarvestTests(unittest.TestCase):
    def harvest(self, missing=()):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            out = base / 'symbols.txt'
            symbols = {}
            for dll, names in harvester.WANTED.items():
                (base / dll).touch()
                symbols[dll] = {name.removeprefix('PREFIX:'): 0x1000 + i * 16
                                for i, name in enumerate(names) if name not in missing}
            stdout, stderr = io.StringIO(), io.StringIO()
            with patch.object(sys, 'argv', ['resolve_symbols.py', str(base), str(out)]), \
                 patch.object(harvester, 'pe_info', side_effect=lambda path: (0x12345678, 0x2000, [], (Path(path).name, 'id'))), \
                 patch.object(harvester, 'fetch_pdb', side_effect=lambda name, sig, cache: name), \
                 patch.object(harvester, 'pdb_publics', side_effect=lambda name, sections: symbols[name]), \
                 contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                result = harvester.main()
            return result, out.read_text().splitlines(), stderr.getvalue()

    def test_all_symbols(self):
        status, lines, errors = self.harvest()
        self.assertEqual(status, 0)
        self.assertEqual(len(lines), 12)
        self.assertTrue(any(line.endswith(TEARDOWN) for line in lines))
        self.assertEqual(errors, '')

    def test_missing_optional_teardown(self):
        status, lines, errors = self.harvest({TEARDOWN})
        self.assertEqual(status, 0)
        self.assertEqual(len(lines), 11)
        self.assertFalse(any(line.endswith(TEARDOWN) for line in lines))
        self.assertIn('optional', errors)

    def test_each_required_symbol_is_still_fatal(self):
        for names in harvester.WANTED.values():
            for name in names:
                if name == TEARDOWN:
                    continue
                with self.subTest(symbol=name):
                    status, lines, errors = self.harvest({name})
                    self.assertEqual(status, 1)
                    self.assertEqual(len(lines), 11)
                    self.assertIn('0 matches', errors)

    def test_optional_absence_does_not_hide_required_failure(self):
        required = harvester.WANTED['twinapi.appcore.dll'][0]
        status, lines, errors = self.harvest({TEARDOWN, required})
        self.assertEqual(status, 1)
        self.assertEqual(len(lines), 10)
        self.assertIn('optional', errors)


if __name__ == '__main__':
    unittest.main()
