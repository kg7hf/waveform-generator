#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Run cmake/wfg_build_info.cmake against temporary Git repositories."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


CMAKE: str
GIT: Path | None
SCRIPT: Path
# inline constexpr <type> <name>{<value>}; one constant per line.
CONSTANT = re.compile(r'^inline constexpr ([\w:]+) (\w+)\{(.+)\};$', re.MULTILINE)
OLD_MTIME_NS = 1_000_000_000 * 1_000_000_000


class BuildInfoTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='wfg-build-info-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.header = self.root / 'build' / 'generated' / 'wfg_build_info.hpp'
        # Fixture repositories ignore the user's and the system Git configuration.
        global_config = self.root / 'gitconfig'
        global_config.write_bytes(b'')
        self.env = {key: value for key, value in os.environ.items()
                    if key not in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY')}
        self.env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=str(global_config),
                        GIT_AUTHOR_NAME='wfg test', GIT_AUTHOR_EMAIL='wfg@example.invalid',
                        GIT_COMMITTER_NAME='wfg test', GIT_COMMITTER_EMAIL='wfg@example.invalid')
        self.repo = self.root / 'repo'
        (self.repo / 'sub').mkdir(parents=True)
        (self.repo / 'tracked.txt').write_bytes(b'tracked\n')
        (self.repo / 'sub' / 'nested.txt').write_bytes(b'nested\n')

    def require_git(self):
        if GIT is None:
            self.skipTest('Git is not available')

    def git(self, *arguments, cwd: Path | None = None) -> str:
        result = subprocess.run([str(GIT), *arguments], cwd=cwd or self.repo, env=self.env,
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def commit(self, cwd: Path | None = None) -> str:
        self.require_git()
        self.git('init', '--quiet', cwd=cwd)
        self.git('add', '--all', cwd=cwd)
        self.git('commit', '--quiet', '--message', 'fixture', cwd=cwd)
        return self.git('rev-parse', 'HEAD', cwd=cwd)

    def run_script(self, source: Path, *, plugins='OFF', plugin_dir: Path | None = None,
                   git: str | None = None) -> dict[str, tuple[str, str]]:
        """The generated header's commit and dirty constants."""
        if git is None:
            git = GIT.as_posix() if GIT else 'GIT_EXECUTABLE-NOTFOUND'
        result = subprocess.run(
            [CMAKE, f'-DWFG_BUILD_INFO_SOURCE_DIR={source.as_posix()}', f'-DWFG_BUILD_INFO_GIT={git}',
             f'-DWFG_BUILD_INFO_PLUGINS={plugins}',
             f'-DWFG_BUILD_INFO_PLUGIN_DIR={plugin_dir.as_posix() if plugin_dir else ""}',
             f'-DWFG_BUILD_INFO_HEADER={self.header.as_posix()}',
             '-P', SCRIPT.as_posix()],
            env=self.env, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        header = self.header.read_text(encoding='utf-8')
        self.assertIn('namespace waveform_generator::build_info\n', header)
        self.assertNotIn('#define', header)
        self.assertNotIn('source_manifest_sha256', header)
        self.assertFalse((self.root / 'build' / 'source-manifest.json').exists())
        constants = {name: (kind, value) for kind, name, value in CONSTANT.findall(header)}
        return constants

    def assert_known(self, info: dict[str, tuple[str, str]], commit: str, dirty: str):
        self.assertRegex(commit, r'^[0-9a-f]{40}$')
        self.assertEqual(info, {
            'fw_commit': ('std::string_view', f'"{commit}"'),
            'fw_commit_hi': ('std::uint32_t', f'0x{commit[0:8]}U'),
            'fw_commit_lo': ('std::uint32_t', f'0x{commit[8:16]}U'),
            'fw_dirty': ('FwDirty', f'FwDirty::{dirty}'),
        })

    def assert_unknown(self, info: dict[str, tuple[str, str]]):
        self.assertEqual(info, {
            'fw_commit': ('std::string_view', '"unknown"'),
            'fw_commit_hi': ('std::uint32_t', '0x00000000U'),
            'fw_commit_lo': ('std::uint32_t', '0x00000000U'),
            'fw_dirty': ('FwDirty', 'FwDirty::unknown'),
        })

    def test_clean_checkout_reports_head(self):
        commit = self.commit()
        self.assert_known(self.run_script(self.repo), commit, 'clean')

    def test_untracked_files_do_not_count(self):
        commit = self.commit()
        (self.repo / 'untracked.txt').write_bytes(b'new\n')
        self.assert_known(self.run_script(self.repo), commit, 'clean')

    def test_modified_or_staged_tracked_file_is_dirty(self):
        commit = self.commit()
        (self.repo / 'sub' / 'nested.txt').write_bytes(b'changed\n')
        self.assert_known(self.run_script(self.repo), commit, 'modified')
        self.git('add', '--all')
        self.assert_known(self.run_script(self.repo), commit, 'modified')

    def test_unknown_without_git(self):
        if GIT is None:
            (self.repo / '.git').mkdir()
        else:
            self.commit()
        self.assert_unknown(self.run_script(self.repo, git='GIT_EXECUTABLE-NOTFOUND'))
        self.assert_unknown(self.run_script(self.repo, plugins='ON', git=''))

    def test_unknown_without_commit_or_own_checkout(self):
        self.commit()
        # The repository encloses sub/, but sub/ is not a checkout of its own.
        self.assert_unknown(self.run_script(self.repo / 'sub'))
        self.assert_unknown(self.run_script(self.root))
        empty = self.root / 'empty'
        empty.mkdir()
        self.git('-C', str(empty), 'init', '--quiet')
        self.assert_unknown(self.run_script(empty))

    def test_plugin_build_never_reports_clean(self):
        commit = self.commit()
        self.assert_known(self.run_script(self.repo, plugins='ON'), commit, 'clean_with_plugins')
        (self.repo / 'untracked.txt').write_bytes(b'new\n')
        self.assert_known(self.run_script(self.repo, plugins='ON'), commit, 'clean_with_plugins')
        (self.repo / 'tracked.txt').write_bytes(b'changed\n')
        self.assert_known(self.run_script(self.repo, plugins='ON'), commit, 'modified')

    def test_header_is_rewritten_only_when_its_content_changes(self):
        commit = self.commit()
        clean = self.run_script(self.repo)
        content = self.header.read_bytes()
        os.utime(self.header, ns=(OLD_MTIME_NS, OLD_MTIME_NS))
        self.assertEqual(self.run_script(self.repo), clean)
        self.assertEqual(self.header.read_bytes(), content)
        self.assertEqual(self.header.stat().st_mtime_ns, OLD_MTIME_NS)
        (self.repo / 'tracked.txt').write_bytes(b'changed\n')
        self.assert_known(self.run_script(self.repo), commit, 'modified')
        self.assertNotEqual(self.header.stat().st_mtime_ns, OLD_MTIME_NS)



if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--git', default='')
    parser.add_argument('--script', type=Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    CMAKE = arguments.cmake
    GIT = Path(arguments.git) if arguments.git and Path(arguments.git).is_file() else None
    SCRIPT = arguments.script.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_arguments])
