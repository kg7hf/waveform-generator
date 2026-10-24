# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check required vendor inputs, build-copy patch isolation and scopes."""

import json
import os
import subprocess
import tempfile
from pathlib import Path
import sys
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from dependencies import check_dependency, prepare, scope

# Set by Git for a hook, for example; they would point every git call of these
# tests (the fixtures' and dependencies.py's) at the enclosing repository.
GIT_REPOSITORY_VARIABLES = ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY')


class DependencyTests(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        for name in GIT_REPOSITORY_VARIABLES:
            os.environ.pop(name, None)
        temporary = tempfile.TemporaryDirectory(prefix='wfg-dependencies-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.child = self.root / 'third_party/example'
        self.child.mkdir(parents=True)
        (self.child / 'source.c').write_bytes(b'old\n')
        self.dep = dict(name='example', destination='third_party/example', files=['source.c'])

    def test_available_dependency(self):
        check_dependency(self.root, self.dep)

    def test_local_changes_are_not_hash_gated(self):
        (self.child / 'source.c').write_bytes(b'changed\n')
        check_dependency(self.root, self.dep)

    def test_missing_source(self):
        (self.child / 'source.c').unlink()
        with self.assertRaisesRegex(ValueError, 'missing dependency file'):
            check_dependency(self.root, self.dep)

    def test_escaping_source_path(self):
        self.dep['files'][0] = '../../outside.c'
        with self.assertRaisesRegex(ValueError, 'escapes'):
            check_dependency(self.root, self.dep)

    def test_missing_patch(self):
        self.dep['patch'] = 'missing.patch'
        with self.assertRaisesRegex(ValueError, 'missing dependency file'):
            check_dependency(self.root, self.dep)

    def test_patch_only_changes_build_copy(self):
        patch = b'--- a/source.c\n+++ b/source.c\n@@ -1 +1 @@\n-old\n+new\n'
        (self.root / 'fix.patch').write_bytes(patch)
        self.dep['patch'] = 'fix.patch'
        check_dependency(self.root, self.dep)
        output = self.root / 'build/vendor'
        prepare(self.root, self.dep, output)
        self.assertEqual((output / 'example/source.c').read_bytes(), b'new\n')
        self.assertEqual((self.child / 'source.c').read_bytes(), b'old\n')
        check_dependency(self.root, self.dep)

    def test_scope(self):
        self.assertEqual(scope(self.dep), 'firmware')
        self.assertEqual(scope(dict(self.dep, scope='firmware')), 'firmware')
        self.assertEqual(scope(dict(self.dep, scope='host-test')), 'host-test')
        with self.assertRaisesRegex(ValueError, "example: unknown scope 'bogus'"):
            scope(dict(self.dep, scope='bogus'))
        with self.assertRaisesRegex(ValueError, 'unknown scope'):
            check_dependency(self.root, dict(self.dep, scope='bogus'))

    def test_firmware_configure_checks_only_firmware_inputs(self):
        # A host-test entry whose submodule is not initialised: the full check
        # fails, the firmware configure (--prepare) checks the firmware entry alone.
        host_test = dict(name='host-only', destination='third_party/host-only', files=['test.hpp'],
                         scope='host-test')
        manifest = json.dumps({'dependencies': [self.dep, host_test]}) + '\n'
        (self.root / 'dependencies.json').write_bytes(manifest.encode())
        command = [sys.executable, str(SCRIPTS / 'dependencies.py'), '--root', str(self.root)]
        full = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(full.returncode, 1, full.stdout)
        self.assertIn('missing dependency file', full.stderr)
        self.assertIn('host-only', full.stderr)
        firmware = subprocess.run([*command, '--prepare', str(self.root / 'build/vendor')],
                                  capture_output=True, text=True)
        self.assertEqual(firmware.returncode, 0, firmware.stderr)
        self.assertEqual(firmware.stdout.strip(), 'Checked 1 local dependencies; prepared vendor patches.')


if __name__ == '__main__':
    unittest.main()
