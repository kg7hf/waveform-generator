# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Check required vendor inputs and build-copy patch isolation."""

import tempfile
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from dependencies import check_dependency, prepare


class DependencyTests(unittest.TestCase):
    def setUp(self):
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


if __name__ == '__main__':
    unittest.main()
