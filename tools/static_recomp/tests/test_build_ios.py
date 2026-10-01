#!/usr/bin/env python3
"""Build-driver contract tests; these do not invoke an Apple SDK or sign an app."""
import importlib.util
import pathlib
import tempfile
import unittest
from unittest.mock import patch

PATH = pathlib.Path(__file__).parents[1] / 'build_ios.py'
SPEC = importlib.util.spec_from_file_location('build_ios', PATH)
driver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(driver)


class BuildDriverTests(unittest.TestCase):
    def test_device_strict_configuration(self):
        with tempfile.TemporaryDirectory() as root:
            root = pathlib.Path(root)
            calls = driver.commands(root / 'build', root / 'catalog', 3)
            self.assertEqual(len(calls), 3)
            self.assertIn('-DXENIA_STATIC_ONLY=ON', calls[1])
            self.assertIn('-DCMAKE_SYSTEM_NAME=iOS', calls[1])
            self.assertIn('-DCMAKE_OSX_SYSROOT=iphoneos', calls[1])
            self.assertIn('-DXENIA_STATIC_MODULES_DIR=' + str(root / 'catalog'), calls[1])
            self.assertIn('CODE_SIGNING_ALLOWED=NO', calls[2])
            self.assertIn('generic/platform=iOS', calls[2])

    def test_empty_catalog_is_explicit(self):
        calls = driver.commands(pathlib.Path('/private/build'), None, 3)
        self.assertIn('-DXENIA_STATIC_MODULES_DIR=', calls[1])

    def test_private_output_boundary(self):
        for path in (driver.ROOT, driver.ROOT / 'private',
                     pathlib.Path('/private/bad;path'), pathlib.Path('/private/bad\npath')):
            with self.assertRaises(ValueError):
                driver.external_path(path, 'test')

    def test_non_mac_fails_before_subprocess_or_writes(self):
        with tempfile.TemporaryDirectory() as root:
            out = pathlib.Path(root) / 'untouched'
            with patch.object(driver.platform, 'system', return_value='Linux'):
                with patch.object(driver.subprocess, 'run') as run:
                    self.assertEqual(driver.main(['--empty-catalog', '--build-dir', str(out)]), 1)
                    run.assert_not_called()
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
