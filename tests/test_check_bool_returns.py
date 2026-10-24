# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-bool-returns.py's rules on synthetic sources, and the ratchet on a fixture repository.

- bool_returns() finds every form of a bool-returning function the gate
  names: a declaration or definition at namespace or class scope, with
  attributes, specifiers, a macro or a template head before it, split over
  lines, qualified, out of its class, an operator, "operator bool", an
  explicit specialisation, a function pointer or member pointer (a member, a
  parameter, a using or typedef alias), a std::function<bool(...)>, a
  trailing "-> bool" and a lambda's, and a deduced return type (a lambda
  without "->", "auto" or "decltype(auto)") whose own body returns the
  literal true or false; each with its namespaces and classes.
  It finds none in a comment, a string or raw string, a preprocessor
  directive, a variable (initialised with =, braces or parentheses), a
  parameter, a cast, a template argument or a function body's locals; nor a
  deduced return of another type or of a bool expression, nor a literal
  return in a function or lambda with a return type of its own.
- is_gated(): the measured paths and signal-lab/validation, without tests.
- parse_baseline() refuses a predicate or abi entry without why, an unknown
  field, an entry listed twice, and an entry in two lists.
- compare(), fallible_listed() and grown(): an unlisted function and a grown
  count fail, a gone entry and a fallen count are stale, and any fallible
  entry fails, since #50 took the list to zero; a fallible entry of the
  previous baseline moved to the predicates or abi list, or a predicate or abi
  count raised, fails unless the "reclassified" list names it with why and
  that count (QA on 9e353af).
- parse_baseline() also refuses a reclassified entry without why, one that
  names no predicate or abi entry, and a list other than the four; it still
  reads a fallible entry, which an earlier baseline may have.
- main() on a fixture repository: a new bool function fails and --update
  will not admit it; a new predicate passes once listed with why; a removed
  function fails until --update drops it; a second overload fails; a fallible
  entry fails whatever the code and whatever HEAD's baseline has, and --update
  will not drop it; a fallible entry moved to predicates or abi, and a raised
  predicate count, pass only when reclassified, which --update keeps with its
  predicate and drops with it; --rev reads the commit, not the work tree, and
  compares it with its parent; --rev reads the commit's own baseline, unless
  --baseline names a file.
- the host CI's ratchet step (.github/workflows/host-ci.yml), its shell run
  on a fixture repository with bash: a push is compared with the main it
  replaced (github.event.before), so a predicate count that a commit before
  the tip raises fails; a pull request and a push that creates the branch are
  compared with the first parent; a push checks out the full history.
"""

from collections import Counter
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'check-bool-returns.py'
_spec = importlib.util.spec_from_file_location('check_bool_returns', SCRIPT)
check_bool_returns = importlib.util.module_from_spec(_spec)
# Its dataclasses look their module up while the class is made.
sys.modules[_spec.name] = check_bool_returns
_spec.loader.exec_module(check_bool_returns)

# The fixture repository ignores the user's and the system's Git configuration
# and carries its own identity.
FIXTURE_GIT_SETTINGS = {
    'GIT_CONFIG_GLOBAL': os.devnull,
    'GIT_CONFIG_NOSYSTEM': '1',
    'GIT_AUTHOR_NAME': 'wfg-test',
    'GIT_AUTHOR_EMAIL': 'wfg-test@example.invalid',
    'GIT_COMMITTER_NAME': 'wfg-test',
    'GIT_COMMITTER_EMAIL': 'wfg-test@example.invalid',
}
GIT_REPOSITORY_VARIABLES = ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY')

# Each source and the names of the bool-returning functions in it, in order.
FOUND = (
    ('bool f();', ['f']),
    ('[[nodiscard]] static constexpr bool f(int x) noexcept;', ['f']),
    ('const bool f();', ['f']),
    ('bool\nf(int value);', ['f']),
    ('namespace a::b\n{\nclass C\n{\npublic:\n    bool g() const;\nprivate:\n    bool h();\n};\n}', ['a::b::C::g', 'a::b::C::h']),
    ('namespace a\n{\nbool C::g() const\n{\n    return true;\n}\n}', ['a::C::g']),
    ('namespace\n{\nbool hidden();\n}', ['hidden']),
    ('struct S\n{\n    bool operator==(const S&) const = default;\n    bool operator()(int) const;\n'
     '    bool operator<(const S&) const;\n    explicit operator bool() const;\n};',
     ['S::operator==', 'S::operator()', 'S::operator<', 'S::operator bool']),
    ('inline Status::operator bool() const noexcept\n{\n    return true;\n}', ['Status::operator bool']),
    ('template <>\nbool parse<int>(int text);', ['parse']),
    ('template <class T>\nrequires std::integral<T>\nbool g(T value);', ['g']),
    ('template <class T, std::size_t N>\nstruct Box\n{\n    bool empty() const;\n};', ['Box::empty']),
    ('class Sink final : public ByteSink\n{\npublic:\n    bool write(int bytes) override;\n};', ['Sink::write']),
    ('struct alignas(16) Aligned final\n{\n    bool ok() const;\n};', ['Aligned::ok']),
    ('struct Reader\n{\n    bool (*read)(void* context, int bytes) noexcept {};\n};', ['Reader::read']),
    ('using Callback = bool (*)(void* context, int bytes);', ['Callback']),
    ('typedef bool (*Legacy)(void);', ['Legacy']),
    ('using Plain = bool(int);', ['Plain']),
    ('bool (Class::*member)(int);', ['member']),
    ('void route(int event, bool (*pending)(void));', ['route::pending']),
    ('void visit(int count, bool keep(int value));', ['visit::keep']),
    ('struct Job\n{\n    std::function<bool(int)> on_done;\n};', ['Job::on_done']),
    ('auto ready() noexcept -> bool;', ['ready']),
    ('int main()\n{\n    auto next = [&](int& value) noexcept -> bool\n    {\n        return value > 0;\n    };\n'
     '    return 0;\n}', ['main::<lambda next>']),
    ('#ifdef __cplusplus\nextern "C" {\n#endif\nTU_ATTR_WEAK bool tud_cb(uint8_t lun);\n'
     'static bool helper(void)\n{\n    return true;\n}\n#ifdef __cplusplus\n}\n#endif', ['tud_cb', 'helper']),
    # A deduced return type that a body's "return true;" or "return false;" makes bool (QA on e661718:
    # signal_lab_render's queue_live_events), once however many such returns; named and lined at its head.
    ('int main()\n{\n    const auto queue = [&]()\n    {\n        while (more())\n        {\n            if (!take())\n'
     '            {\n                return false;\n            }\n        }\n\n        return true;\n    };\n    return 0;\n}',
     ['main::<lambda queue>']),
    ('void f()\n{\n    run([](int x) mutable noexcept { return false; });\n    run([] { return true; });\n}',
     ['f::<lambda>', 'f::<lambda>']),
    # Its only literal return in a block of its body.
    ('namespace a\n{\nauto ready(int x) noexcept\n{\n    if (x > 0)\n    {\n        return true;\n    }\n\n'
     '    return x < 0;\n}\n\nstatic decltype(auto) S::done() const\n{\n    return false;\n}\n}', ['a::ready', 'a::S::done']),
    # Found at its return, listed at its head: before a lambda its body declares.
    ('auto outer()\n{\n    auto inner = [](int x) -> bool\n    {\n        return x > 0;\n    };\n    return true;\n}',
     ['outer', 'outer::<lambda inner>']),
    ('auto outer()\n{\n    auto inner = []() -> int\n    {\n        return 1;\n    };\n    return true;\n}', ['outer']),
    ('struct S\n{\n    auto operator[](int i) const\n    {\n        return true;\n    }\n\n    auto operator()(int i) const\n    {\n'
     '        return false;\n    }\n};', ['S::operator[]', 'S::operator()']),
    # The "return true;" of a lambda with "-> bool" is that lambda's, not its deduced function's.
    ('auto f()\n{\n    auto inner = [](int x) -> bool\n    {\n        return true;\n    };\n    return 1;\n}', ['f::<lambda inner>']),
)

# Sources in which no function returns bool.
IGNORED = (
    '// bool f();',
    '/* bool f(int);\n   bool g(); */',
    'const char* text = "bool f();";',
    'const char* raw = R"(bool f();)";',
    '#define DECLARE bool f(void)',
    '#define LONGER(name) \\\n    bool name(void)',
    'bool flag = check(1);',
    'bool flag{false};',
    'static bool flag(true);',
    'static bool flag(0);',
    'void set(bool on, bool off);',
    'auto x = static_cast<bool>(y);',
    'std::atomic<bool> flag;',
    'template <bool Enable>\nstruct T\n{\n};',
    'std::optional<bool> maybe();',
    'std::pair<bool, int> both();',
    'struct Bits\n{\n    bool on : 1;\n};',
    'static_assert(bool(X));',
    'constexpr bool value = bool(1);',
    'void f()\n{\n    bool ok(check());\n    bool (*local)(int) = nullptr;\n    return bool(ok);\n}',
    # A deduced return of another type, or of a bool expression; a "return true;" that belongs to a function
    # or lambda with a return type of its own, or to a block of one.
    'auto f()\n{\n    return 1;\n}\nauto g = [](int x) { return x > 0; };',
    'int f(int x)\n{\n    if (x)\n    {\n        return true;\n    }\n\n    return false;\n}',
    'Status Reader::read() const\n{\n    for (int i : items)\n    {\n        return false;\n    }\n}',
    'int values[2] {1, 2};\nauto pick = values[0];',
)


def names(text: str) -> list[str]:
    return [found.name for found in check_bool_returns.bool_returns('common/a.hpp', text + '\n')]


def baseline_text(fallible=(), predicates=(), abi=(), reclassified=None) -> str:
    def entries(rows):
        return [dict(row) for row in rows]
    lists = {'fallible': entries(fallible), 'predicates': entries(predicates), 'abi': entries(abi)}
    if reclassified is not None:
        lists['reclassified'] = entries(reclassified)
    return json.dumps(lists, indent=2) + '\n'


class ScannerTests(unittest.TestCase):
    def test_every_form_is_found(self):
        for text, expected in FOUND:
            with self.subTest(text=text):
                self.assertEqual(names(text), expected)

    def test_no_ignored_form_is_found(self):
        for text in IGNORED:
            with self.subTest(text=text):
                self.assertEqual(names(text), [])

    def test_each_declaration_names_the_line_of_its_bool(self):
        found = check_bool_returns.bool_returns('rt1170/a.cpp', '// one\nint g();\n[[nodiscard]]\nbool\nf();\nbool h();\n')
        self.assertEqual([(item.file, item.name, item.line) for item in found],
                         [('rt1170/a.cpp', 'f', 4), ('rt1170/a.cpp', 'h', 6)])

    def test_a_declaration_and_its_definition_count_twice(self):
        found = check_bool_returns.bool_returns('rt1170/a.cpp', 'bool f(int);\nbool f(int value)\n{\n    return value;\n}\n')
        self.assertEqual(Counter(item.key for item in found), Counter({('rt1170/a.cpp', 'f'): 2}))

    def test_the_gated_sources_are_the_measured_paths_without_tests(self):
        for path in ('rt1170/player.cpp', 'host/capture/capture_main.cpp', 'signal-lab/validation/watterson_render.cpp',
                     'channel-sim/cli/options.cpp', 'common/dd008/cdc_mux_session.hpp', 'waveform-source/encoder.hpp'):
            with self.subTest(path=path):
                self.assertTrue(check_bool_returns.is_gated(path))
        for path in ('waveform-source/tests.cpp', 'rt1170/player_tests.cpp', 'common/wav_test.cpp', 'signal-lab/src/test_x.cpp',
                     'tests/contract_tests.cpp', 'signal-lab/tests/engine.cpp', 'channel-sim/tests/chain.cpp',
                     'third_party/tinyusb/src/tusb.c', 'host/other/tool.cpp'):
            with self.subTest(path=path):
                self.assertFalse(check_bool_returns.is_gated(path))


class BaselineTests(unittest.TestCase):
    def parse(self, **lists):
        return check_bool_returns.parse_baseline(baseline_text(**lists), 'test.json')

    def test_a_fallible_entry_is_read_with_no_why_and_counts_one_by_default(self):
        # An earlier revision's baseline, which the reclassification guard compares with, may still list some.
        baseline = self.parse(fallible=[{'file': 'a.cpp', 'function': 'f'}])
        self.assertEqual(baseline.fallible[('a.cpp', 'f')].count, 1)

    def test_malformed_baselines_are_refused(self):
        cases = {
            'a predicate without why': dict(predicates=[{'file': 'a.cpp', 'function': 'f'}]),
            'an abi entry without why': dict(abi=[{'file': 'a.c', 'function': 'tud_cb', 'why': ' '}]),
            'an unknown field': dict(fallible=[{'file': 'a.cpp', 'function': 'f', 'class': 'fallible'}]),
            'a count of none': dict(fallible=[{'file': 'a.cpp', 'function': 'f', 'count': 0}]),
            'listed twice': dict(fallible=[{'file': 'a.cpp', 'function': 'f'}, {'file': 'a.cpp', 'function': 'f'}]),
            'in two lists': dict(fallible=[{'file': 'a.cpp', 'function': 'f'}],
                                 predicates=[{'file': 'a.cpp', 'function': 'f', 'why': 'a question'}]),
        }
        for case, lists in cases.items():
            with self.subTest(case=case), self.assertRaises(ValueError):
                self.parse(**lists)
        with self.assertRaises(ValueError):
            check_bool_returns.parse_baseline('{"fallible": [], "predicates": []}', 'test.json')

    def test_unlisted_and_grown_fail_gone_and_fallen_are_stale(self):
        baseline = self.parse(fallible=[{'file': 'a.cpp', 'function': 'read', 'count': 2}, {'file': 'a.cpp', 'function': 'gone'},
                                        {'file': 'a.cpp', 'function': 'write'}],
                              predicates=[{'file': 'a.cpp', 'function': 'empty', 'why': 'a question'}])
        found = [check_bool_returns.Found('a.cpp', name, line) for name, line in
                 (('read', 1), ('write', 2), ('write', 3), ('empty', 4), ('empty', 5), ('open', 6))]
        outcome = check_bool_returns.compare(found, baseline)
        self.assertEqual(len(outcome.failures), 3, outcome.failures)
        self.assertIn('a.cpp:2: write: 2 bool-returning declarations, its "fallible" entry counts 1: the fallible list '
                      'stays empty', outcome.failures)
        self.assertTrue(any(line.startswith('a.cpp:4: empty: 2 ') for line in outcome.failures), outcome.failures)
        self.assertTrue(any(line.startswith('a.cpp:6: open: returns bool and is not in the baseline')
                            for line in outcome.failures), outcome.failures)
        self.assertEqual(outcome.stale, ['a.cpp: read: 1 bool-returning declarations, its "fallible" entry counts 2',
                                         'a.cpp: gone: listed under "fallible", but returns no bool or is gone'])

    def test_any_fallible_entry_fails(self):
        # #50 took the fallible list to zero: an entry fails whatever the code, and whatever the previous baseline had.
        self.assertEqual(check_bool_returns.fallible_listed(self.parse(predicates=[{'file': 'a.cpp', 'function': 'empty',
                                                                                    'why': 'a question'}])), [])
        listed = self.parse(fallible=[{'file': 'b.cpp', 'function': 'read'}, {'file': 'a.cpp', 'function': 'write', 'count': 2}])
        remedy = ('which stays empty (#50 took it to zero): return a Status or a Result (owned C code an enum wfg_status); '
                  'if its false is an answer, never a failure, list it under "predicates" with why')
        self.assertEqual(check_bool_returns.fallible_listed(listed), [f'a.cpp: write: listed under "fallible", {remedy}',
                                                                      f'b.cpp: read: listed under "fallible", {remedy}'])
        # The comparison with the previous baseline leaves the fallible list to this rule: one listed there too
        # still fails here.
        self.assertEqual(check_bool_returns.grown(listed, listed, 'HEAD'), [])
        self.assertEqual(check_bool_returns.grown(listed, self.parse(), 'HEAD'), [])

    def test_a_reclassification_shows_in_the_reclassified_list(self):
        previous = self.parse(fallible=[{'file': 'a.c', 'function': 'init'}],
                              predicates=[{'file': 'a.cpp', 'function': 'empty', 'why': 'a question'}],
                              abi=[{'file': 'a.c', 'function': 'tud_cb', 'why': 'TinyUSB\'s callback'}])
        init_ready = {'file': 'a.c', 'function': 'init', 'why': 'whether the card is ready'}
        empty_twice = {'file': 'a.cpp', 'function': 'empty', 'count': 2, 'why': 'a question'}
        changed = dict(predicates=[init_ready, empty_twice], abi=[{'file': 'a.c', 'function': 'tud_cb', 'why': 'the callback'}])
        failures = check_bool_returns.grown(self.parse(**changed), previous, 'HEAD')
        self.assertEqual(failures, [
            'a.c: init: a fallible entry in HEAD\'s baseline, now under "predicates": list it under "reclassified" with '
            'why (and its count), so the change shows in review; or return a Status or a Result',
            'a.cpp: empty: a "predicates" count of 2, HEAD\'s baseline has 1: list it under "reclassified" with why (and its '
            'count), so the change shows in review; or return a Status or a Result'])
        named = [{'file': 'a.c', 'function': 'init', 'why': 'its false says not ready'},
                 {'file': 'a.cpp', 'function': 'empty', 'count': 2, 'why': 'an overload for a span'}]
        self.assertEqual(check_bool_returns.grown(self.parse(**changed, reclassified=named), previous, 'HEAD'), [])
        # A reclassified entry that counts fewer declarations than its predicate does not admit it; a new
        # entry, and a move between the predicates and abi lists, need none.
        short = [named[0], {**named[1], 'count': 1}]
        self.assertEqual(len(check_bool_returns.grown(self.parse(**changed, reclassified=short), previous, 'HEAD')), 1)
        moved = self.parse(predicates=[{'file': 'a.c', 'function': 'tud_cb', 'why': 'a question'},
                                       {'file': 'b.cpp', 'function': 'is_new', 'why': 'a new question'}],
                           abi=[{'file': 'a.cpp', 'function': 'empty', 'why': 'a callback'}])
        self.assertEqual(check_bool_returns.grown(moved, previous, 'HEAD'), [])

    def test_a_reclassified_entry_names_a_predicate_or_abi_entry_and_why(self):
        listed = dict(predicates=[{'file': 'a.cpp', 'function': 'f', 'why': 'a question'}])
        self.assertEqual(list(self.parse(**listed, reclassified=[{'file': 'a.cpp', 'function': 'f', 'why': 'x'}]).reclassified),
                         [('a.cpp', 'f')])
        for case, entries in {'no why': [{'file': 'a.cpp', 'function': 'f'}],
                              'no predicate': [{'file': 'a.cpp', 'function': 'g', 'why': 'x'}],
                              'listed twice': [{'file': 'a.cpp', 'function': 'f', 'why': 'x'}] * 2}.items():
            with self.subTest(case=case), self.assertRaises(ValueError):
                self.parse(**listed, reclassified=entries)
        with self.assertRaises(ValueError):
            check_bool_returns.parse_baseline('{"fallible": [], "predicates": [], "abi": [], "notes": []}', 'test.json')

    def test_update_writes_one_entry_per_line_and_drops_what_is_gone(self):
        baseline = self.parse(predicates=[{'file': 'a.cpp', 'function': 'is_open', 'count': 2, 'why': 'whether it is open'},
                                          {'file': 'a.cpp', 'function': 'gone', 'why': 'a question'}],
                              abi=[{'file': 'a.c', 'function': 'tud_cb', 'why': 'TinyUSB\'s callback'}])
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'baseline.json'
            check_bool_returns.write_baseline(target, baseline, Counter({('a.cpp', 'is_open'): 1, ('a.c', 'tud_cb'): 1}))
            data = target.read_bytes()
        self.assertNotIn(b'\r', data)
        self.assertEqual(data.decode('utf-8'),
                         '{\n  "fallible": [\n  ],\n'
                         '  "predicates": [\n    {"file": "a.cpp", "function": "is_open", "why": "whether it is open"}\n  ],\n'
                         '  "abi": [\n    {"file": "a.c", "function": "tud_cb", "why": "TinyUSB\'s callback"}\n  ]\n}\n')
        reread = check_bool_returns.parse_baseline(data.decode('utf-8'), 'baseline.json')
        self.assertEqual(list(reread.fallible), [])
        self.assertEqual(list(reread.lists['predicates']), [('a.cpp', 'is_open')])


class FixtureRepository(unittest.TestCase):
    """A fixture repository with one source of a function that returns a Status and a predicate, and its baseline."""

    BASELINE = 'scripts/bool-returns-baseline.json'
    CARD = 'm110::Status read_card(int n);\nbool is_ready();\n'
    # read_card as it was before #50: bool, and listed as fallible.
    READ = {'file': 'rt1170/card.cpp', 'function': 'read_card', 'why': 'false: a failed read'}
    READY = {'file': 'rt1170/card.cpp', 'function': 'is_ready', 'why': 'whether the card is ready'}

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = {key: value for key, value in os.environ.items() if key not in GIT_REPOSITORY_VARIABLES}
        self.environment.update(FIXTURE_GIT_SETTINGS)
        self.git('init', '-q')
        self.write('rt1170/card.cpp', self.CARD)
        self.write_baseline(predicates=[self.READY])

    def git(self, *args):
        subprocess.run(['git', '-C', str(self.root), *args], check=True, capture_output=True, env=self.environment)

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode('utf-8'))
        self.git('add', path)

    def write_baseline(self, **lists):
        self.write(self.BASELINE, baseline_text(**lists))

    def baseline_bytes(self):
        return (self.root / self.BASELINE).read_bytes()

    def run_gate(self, *extra):
        return subprocess.run([sys.executable, str(SCRIPT), '--root', str(self.root), *extra], capture_output=True, text=True,
                              env=self.environment)

    def assert_exit(self, result, status):
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)


class GateTests(FixtureRepository):
    """The script on a fixture repository: its exit status, its baseline and the revisions it compares."""

    def test_a_listed_tree_passes_and_a_new_bool_function_fails(self):
        self.git('commit', '-q', '-m', 'gate')
        passed = self.run_gate('--list')
        self.assert_exit(passed, 0)
        self.assertIn('rt1170/card.cpp:2: is_ready  [predicates]', passed.stdout)
        self.assertNotIn('read_card', passed.stdout)
        self.assertIn('0 fallible entries (0 declarations; the list stays empty, #50)', passed.stdout)
        self.write('rt1170/card.cpp', self.CARD + 'bool write_card(int n);\n')
        failed = self.run_gate()
        self.assert_exit(failed, 1)
        self.assertIn('FAIL rt1170/card.cpp:3: write_card: returns bool and is not in the baseline', failed.stdout)
        before = self.baseline_bytes()
        self.assert_exit(self.run_gate('--update'), 1)
        self.assertEqual(self.baseline_bytes(), before)

    def test_a_new_predicate_passes_once_listed_with_why(self):
        self.git('commit', '-q', '-m', 'gate')
        self.write('rt1170/card.cpp', self.CARD + 'bool has_card();\n')
        self.assert_exit(self.run_gate(), 1)
        has_card = {'file': 'rt1170/card.cpp', 'function': 'has_card'}
        self.write_baseline(predicates=[self.READY, has_card])
        self.assert_exit(self.run_gate(), 2)
        self.write_baseline(predicates=[self.READY, {**has_card, 'why': 'whether a card is in'}])
        self.assert_exit(self.run_gate(), 0)

    def test_comments_strings_tests_and_vendored_code_need_no_entry(self):
        self.write('rt1170/card.cpp', self.CARD + '// bool old_read(int n);\nconst char* text = "bool f();";\n')
        self.write('waveform-source/tests.cpp', 'bool check(int value);\n')
        self.write('tests/card_tests.cpp', 'bool check(int value);\n')
        self.write('third_party/lib/vendor.c', 'bool vendor_init(void);\n')
        self.write('rt1170/image.hpp', '\0bool embedded();\n')  # a binary file is no source
        self.assert_exit(self.run_gate(), 0)

    def test_a_removed_function_fails_until_update_drops_it(self):
        self.git('commit', '-q', '-m', 'gate')
        self.write('rt1170/card.cpp', 'm110::Status read_card(int n);\n')
        failed = self.run_gate()
        self.assert_exit(failed, 1)
        self.assertIn('FAIL rt1170/card.cpp: is_ready: listed under "predicates", but returns no bool or is gone: run --update',
                      failed.stdout)
        updated = self.run_gate('--update')
        self.assert_exit(updated, 0)
        self.assertIn('drop rt1170/card.cpp: is_ready', updated.stdout)
        self.assertNotIn(b'is_ready', self.baseline_bytes())
        self.assert_exit(self.run_gate(), 0)

    def test_a_second_overload_of_a_listed_name_fails(self):
        self.git('commit', '-q', '-m', 'gate')
        self.write('rt1170/card.cpp', self.CARD + 'bool is_ready(int slot);\n')
        failed = self.run_gate()
        self.assert_exit(failed, 1)
        self.assertIn('2 bool-returning declarations, its "predicates" entry counts 1: raise its count only if the new one is '
                      'under "predicates" too', failed.stdout)
        self.assert_exit(self.run_gate('--update'), 1)

    def test_any_fallible_entry_fails(self):
        # Listed by hand, a new bool function fails whatever HEAD's baseline had, and --update will not admit it.
        self.git('commit', '-q', '-m', 'gate')
        write = {'file': 'rt1170/card.cpp', 'function': 'write_card'}
        self.write('rt1170/card.cpp', self.CARD + 'bool write_card(int n);\n')
        self.write_baseline(fallible=[write], predicates=[self.READY])
        failed = self.run_gate()
        self.assert_exit(failed, 1)
        self.assertIn('FAIL rt1170/card.cpp: write_card: listed under "fallible", which stays empty (#50 took it to zero)',
                      failed.stdout)
        before = self.baseline_bytes()
        self.assert_exit(self.run_gate('--update'), 1)
        self.assertEqual(self.baseline_bytes(), before)
        # Nor may a predicate become fallible.
        self.write('rt1170/card.cpp', self.CARD)
        self.write_baseline(fallible=[{'file': 'rt1170/card.cpp', 'function': 'is_ready'}])
        self.assertIn('is_ready: listed under "fallible", which stays empty', self.run_gate().stdout)
        # A fallible entry fails even where HEAD's baseline has it, and even with its function gone.
        self.write('rt1170/card.cpp', 'bool read_card(int n);\nbool is_ready();\n')
        self.write_baseline(fallible=[self.READ], predicates=[self.READY])
        self.git('commit', '-q', '-m', 'a fallible entry before #50')
        self.assertIn('read_card: listed under "fallible", which stays empty', self.run_gate().stdout)
        self.write('rt1170/card.cpp', self.CARD)
        gone = self.run_gate('--update')
        self.assert_exit(gone, 1)
        self.assertIn('read_card: listed under "fallible", which stays empty', gone.stdout)

    def test_a_fallible_entry_moved_to_predicates_or_abi_needs_a_reclassified_entry(self):
        # QA on 9e353af: moving the fallible entries to "predicates" with a why emptied the list, code unchanged.
        # HEAD is a revision from before #50 took the list to zero.
        self.write('rt1170/card.cpp', 'bool read_card(int n);\nbool is_ready();\n')
        self.write_baseline(fallible=[self.READ], predicates=[self.READY])
        self.git('commit', '-q', '-m', 'a fallible entry before #50')
        read_as_predicate = {**self.READ, 'why': 'whether the card reads'}
        for kind in ('predicates', 'abi'):
            with self.subTest(kind=kind):
                self.write_baseline(predicates=[self.READY, *([read_as_predicate] if kind == 'predicates' else [])],
                                    abi=[read_as_predicate] if kind == 'abi' else [])
                moved = self.run_gate()
                self.assert_exit(moved, 1)
                self.assertIn(f'FAIL rt1170/card.cpp: read_card: a fallible entry in HEAD\'s baseline, now under "{kind}": '
                              f'list it under "reclassified"', moved.stdout)
        # Named under "reclassified" with why, the move passes, and the list shows it.
        reclassified = {'file': 'rt1170/card.cpp', 'function': 'read_card', 'why': 'its false means no card, never a failure'}
        self.write_baseline(predicates=[self.READY, read_as_predicate], reclassified=[reclassified])
        passed = self.run_gate()
        self.assert_exit(passed, 0)
        self.assertIn('1 reclassified', passed.stdout)

    def test_a_raised_predicate_count_needs_a_reclassified_entry(self):
        self.git('commit', '-q', '-m', 'gate')
        self.write('rt1170/card.cpp', self.CARD + 'bool is_ready(int slot);\n')
        self.write_baseline(predicates=[{**self.READY, 'count': 2}])
        raised = self.run_gate()
        self.assert_exit(raised, 1)
        self.assertIn('FAIL rt1170/card.cpp: is_ready: a "predicates" count of 2, HEAD\'s baseline has 1', raised.stdout)
        ready = {'file': 'rt1170/card.cpp', 'function': 'is_ready', 'why': 'whether a given slot is ready too'}
        self.write_baseline(predicates=[{**self.READY, 'count': 2}], reclassified=[ready])
        self.assert_exit(self.run_gate(), 1)  # it admits one declaration unless it counts more
        self.write_baseline(predicates=[{**self.READY, 'count': 2}], reclassified=[{**ready, 'count': 2}])
        self.assert_exit(self.run_gate(), 0)
        # --update keeps it while its predicate is listed, and drops it with its predicate.
        self.assert_exit(self.run_gate('--update'), 0)
        self.assertIn(b'"reclassified"', self.baseline_bytes())
        self.write('rt1170/card.cpp', 'm110::Status read_card(int n);\n')
        self.assert_exit(self.run_gate('--update'), 0)
        self.assertNotIn(b'is_ready', self.baseline_bytes())
        self.assertNotIn(b'"reclassified"', self.baseline_bytes())

    def test_rev_reads_the_commit_and_compares_it_with_its_parent(self):
        self.git('commit', '-q', '-m', 'gate')
        first = self.run_gate('--rev', 'HEAD')
        self.assert_exit(first, 0)
        self.assertIn('not compared with an earlier baseline: HEAD^1 is not a commit here', first.stdout)
        # A commit that raises a predicate's count without a reclassified entry: --rev compares it with its parent.
        self.write('rt1170/card.cpp', self.CARD + 'bool is_ready(int slot);\n')
        self.write_baseline(predicates=[{**self.READY, 'count': 2}])
        self.git('commit', '-q', '-m', 'an overload more')
        self.assert_exit(self.run_gate(), 0)  # the work tree is HEAD's, compared with HEAD
        raised = self.run_gate('--rev', 'HEAD')
        self.assert_exit(raised, 1)
        self.assertIn('tree of HEAD', raised.stdout)
        self.assertIn('is_ready: a "predicates" count of 2, HEAD^1\'s baseline has 1', raised.stdout)
        self.assert_exit(self.run_gate('--rev', 'HEAD^1'), 0)
        # A commit that lists a fallible function fails at that commit.
        self.write('rt1170/card.cpp', self.CARD + 'bool is_ready(int slot);\nbool write_card(int n);\n')
        self.write_baseline(fallible=[{'file': 'rt1170/card.cpp', 'function': 'write_card'}], predicates=[{**self.READY, 'count': 2}])
        self.git('commit', '-q', '-m', 'a fallible function more')
        listed = self.run_gate('--rev', 'HEAD')
        self.assert_exit(listed, 1)
        self.assertIn('write_card: listed under "fallible", which stays empty', listed.stdout)
        # --rev reads the commit's sources and baseline, not the work tree's.
        self.write('rt1170/card.cpp', 'bool is_ready();\n')
        self.assert_exit(self.run_gate(), 1)
        self.assert_exit(self.run_gate('--rev', 'HEAD~2'), 0)

    def test_rev_reads_its_own_baseline_unless_one_is_named(self):
        # The commit's baseline lists is_ready; the work tree's lists nothing, so it fails the work tree.
        self.git('commit', '-q', '-m', 'gate')
        self.write_baseline()
        self.assert_exit(self.run_gate(), 1)
        own = self.run_gate('--rev', 'HEAD')
        self.assert_exit(own, 0)
        self.assertIn('1 predicates (1)', own.stdout)
        # --baseline names the file to read, with --rev too: here the work tree's, which fails the commit.
        named = self.run_gate('--rev', 'HEAD', '--baseline', str(self.root / self.BASELINE))
        self.assert_exit(named, 1)
        self.assertIn('FAIL rt1170/card.cpp:2: is_ready: returns bool and is not in the baseline', named.stdout)
        self.assertIn('tree of HEAD', named.stdout)
        elsewhere = self.root / 'elsewhere.json'
        elsewhere.write_bytes(baseline_text(predicates=[self.READY]).encode('utf-8'))
        self.assert_exit(self.run_gate('--rev', 'HEAD', '--baseline', str(elsewhere)), 0)

    def test_update_takes_no_rev_and_a_missing_baseline_cannot_run(self):
        self.git('commit', '-q', '-m', 'gate')
        self.assert_exit(self.run_gate('--update', '--rev', 'HEAD'), 2)
        (self.root / self.BASELINE).unlink()
        missing = self.run_gate()
        self.assert_exit(missing, 2)
        self.assertIn('failed to run', missing.stderr)


class WorkflowTests(FixtureRepository):
    """The host CI's ratchet step (.github/workflows/host-ci.yml), its shell run on a fixture repository: a push
    is compared with the main it replaced, so a change the guard checks that a commit before the tip makes fails."""

    WORKFLOW = Path(__file__).resolve().parents[1] / '.github' / 'workflows' / 'host-ci.yml'
    STEP = '- name: Bool-return ratchet'

    def step_script(self) -> str:
        """The step's run (a "run: |" block, unindented, or one line), its python the one running this test."""
        text = self.WORKFLOW.read_text(encoding='utf-8')
        step = text[text.index(self.STEP):]
        step = step[:step.find('\n      - name:')]
        run = step[step.index('run:') + len('run:'):]
        if run.startswith(' |'):
            script = '\n'.join(line[10:] for line in run.splitlines()[1:] if line.strip())
        else:
            script = run.strip()
        return script.replace('python scripts/', f'"{Path(sys.executable).as_posix()}" scripts/') + '\n'

    def run_step(self, event: str, before: str) -> subprocess.CompletedProcess:
        environment = {**self.environment, 'EVENT_NAME': event, 'BEFORE': before}
        return subprocess.run([shutil.which('bash'), '-c', self.step_script()], cwd=self.root, capture_output=True, text=True,
                              env=environment)

    def head(self) -> str:
        return subprocess.run(['git', '-C', str(self.root), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                              env=self.environment, check=True).stdout.strip()

    @unittest.skipUnless(shutil.which('bash'), 'needs bash, as the CI runner has')
    def test_a_push_is_compared_with_the_main_it_replaced(self):
        for script in ('check-bool-returns.py', 'check-style.py', 'check-complexity.py'):
            self.write(f'scripts/{script}', (SCRIPT.parent / script).read_text(encoding='utf-8'))
        self.git('commit', '-q', '-m', 'main before the push')
        before = self.head()
        # The push's first commit raises a predicate's count with no reclassified entry; its second changes nothing
        # the gate reads.
        self.write('rt1170/card.cpp', self.CARD + 'bool is_ready(int slot);\n')
        self.write_baseline(predicates=[{**self.READY, 'count': 2}])
        self.git('commit', '-q', '-m', 'an overload more')
        self.write('docs/notes.md', 'unrelated\n')
        self.git('commit', '-q', '-m', 'unrelated')
        pushed = self.run_step('push', before)
        self.assert_exit(pushed, 1)
        self.assertIn(f'is_ready: a "predicates" count of 2, {before}\'s baseline has 1', pushed.stdout)
        # A pull request's merge commit (whose event's "before" is the branch's previous head), and a push that
        # creates the branch, are compared with the first parent: its tip changes nothing, and a tip that raises
        # the count again fails.
        self.assert_exit(self.run_step('pull_request', before), 0)
        self.assert_exit(self.run_step('push', '0' * 40), 0)
        self.write('rt1170/card.cpp', self.CARD + 'bool is_ready(int slot);\nbool is_ready(long slot);\n')
        self.write_baseline(predicates=[{**self.READY, 'count': 3}])
        self.git('commit', '-q', '-m', 'another at the tip')
        for event, pushed_over in (('pull_request', before), ('push', '0' * 40)):
            with self.subTest(event=event):
                tip = self.run_step(event, pushed_over)
                self.assert_exit(tip, 1)
                self.assertIn('is_ready: a "predicates" count of 3, HEAD^1\'s baseline has 2', tip.stdout)

    def test_a_push_checks_out_the_history_it_compares_with(self):
        text = self.WORKFLOW.read_text(encoding='utf-8')
        checks = text[text.index('  checks:'):text.index('  host:')]
        self.assertIn("fetch-depth: ${{ github.event_name == 'push' && '0' || '2' }}", checks)
        self.assertIn('BEFORE: ${{ github.event.before }}', checks)


if __name__ == '__main__':
    unittest.main()
