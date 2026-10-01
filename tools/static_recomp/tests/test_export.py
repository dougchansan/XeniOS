#!/usr/bin/env python3
"""Offline exporter/catalog tests use only self-authored PowerPC words."""
import hashlib
import importlib.util
import json
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


exporter = load_tool('export')
linker = load_tool('link')
CODE = struct.pack('>II', 0x3860002A, 0x4E800020)  # li r3,42; blr


class ExportTests(unittest.TestCase):
    def test_export_and_source_integrity(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory) / 'generated'
            report = exporter.export_image('fixture', [(0x82010000, CODE)], out)
            self.assertEqual(report['instructions_supported'], 2)
            self.assertFalse(report['gameplay_verified'])
            self.assertIn('e.Tick', next(out.glob('*_00000.cc')).read_text())
            info = json.loads((out / 'module.json').read_text())
            for name, sha in info['sources'].items():
                self.assertEqual(hashlib.sha256((out / name).read_bytes()).hexdigest(), sha)
            with self.assertRaises(ValueError):
                exporter.export_image('fixture', [(0x1000, CODE)], out)

    def test_unsupported_rejects_transactionally(self):
        with tempfile.TemporaryDirectory() as directory:
            out = pathlib.Path(directory) / 'generated'
            with self.assertRaises(ValueError):
                exporter.export_image('fixture', [(0x1000, b'\0' * 4)], out)
            self.assertFalse(out.exists())
            report = exporter.export_image('fixture', [(0x1000, b'\0' * 4)], out, True)
            self.assertEqual(len(report['unsupported']), 1)
            self.assertIn('kUnsupportedInstruction', next(out.glob('*_00000.cc')).read_text())

    def test_unintegrated_reservations_are_not_claimed_supported(self):
        for word in (0x7C602028, 0x7C60212D):  # lwarx; stwcx.
            with self.assertRaises(exporter.Unsupported):
                exporter.decode(word, 0x1000)

    def test_manifest_hash_and_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'code.bin').write_bytes(CODE)
            manifest = {'format': exporter.FORMAT, 'abi': 1, 'name': 'fixture',
                        'ranges': [{'base': 0x1000, 'size': len(CODE), 'file': 'code.bin',
                                    'sha256': hashlib.sha256(CODE).hexdigest()}]}
            path = root / 'image.json'
            path.write_text(json.dumps(manifest))
            self.assertEqual(exporter.load_image(path)[1], [(0x1000, CODE)])
            manifest['ranges'][0]['sha256'] = '0' * 64
            path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                exporter.load_image(path)
            manifest['ranges'][0]['file'] = '../code.bin'
            path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                exporter.load_image(path)

    def test_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            module = root / 'module'
            exporter.export_image('fixture', [(0x1000, CODE)], module)
            out = root / 'catalog'
            self.assertEqual(linker.link([module], out), 1)
            self.assertIn('LinkedModules', (out / 'catalog.cc').read_text())
            self.assertIn('CMAKE_CURRENT_LIST_DIR', (out / 'catalog.cmake').read_text())
            self.assertFalse(json.loads((out / 'catalog.json').read_text())['gameplay_verified'])
            with self.assertRaises(ValueError):
                linker.link([module], out)
            with self.assertRaises(ValueError):
                linker.link([module, module], root / 'duplicate')
            self.assertFalse((root / 'duplicate').exists())

    def test_catalog_rejects_changed_or_extra_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            module = root / 'module'
            exporter.export_image('fixture', [(0x1000, CODE)], module)
            source = next(module.glob('*.cc'))
            original = source.read_bytes()
            source.write_bytes(original + b'\n// changed\n')
            with self.assertRaises(ValueError):
                linker.link([module], root / 'tampered')
            source.write_bytes(original)
            (module / 'unexpected.cc').write_text('// not in source manifest\n')
            with self.assertRaises(ValueError):
                linker.link([module], root / 'extra')
            self.assertFalse((root / 'tampered').exists())
            self.assertFalse((root / 'extra').exists())


if __name__ == '__main__':
    unittest.main()
