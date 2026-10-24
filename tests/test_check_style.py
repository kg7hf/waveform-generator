# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-style.py's brace rules on synthetic K&R and Allman sources, in a fixture repository.

Every K&R line below must be flagged, including a function head that ends in a
qualifier (noexcept, const, override, final), a trailing return type (lambdas
included) or a template class head, and the rarer forms: a constructor's member
initialiser in braces, a lambda without a parameter list, try, a requires
clause, typedef/inline/export heads, a qualifier alone on a continuation line,
a brace followed or preceded by a comment, and a type head with parentheses in
it (an attribute's arguments, alignas(), __attribute__(()), a decltype() in a
base class). No Allman line may be, nor
'extern "C" {', nor an initialiser after a member reached through a call
('f()->m = {'), nor a line of a comment or of a string or character literal that
ends in ") {" (a raw string's lines and a backslash-continued // comment
included). Digit separators and escaped quotes do not open or close a literal.
"""

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'check-style.py'
_spec = importlib.util.spec_from_file_location('check_style', SCRIPT)
check_style = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_style)

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

# One K&R opening brace per line; each line must be flagged.
K_AND_R = (
    'void f() {',
    'void f() noexcept {',
    'void f() const noexcept {',
    'void f() const {',
    'void f() noexcept(true) {',
    'void f() noexcept(noexcept(g())) {',
    'void f() override {',
    'void f() final {',
    'void f() const override {',
    'void f() & {',
    'void f() && noexcept {',
    'auto f() -> int {',
    'auto f() noexcept -> std::array<int, 2> {',
    'auto f() const -> decltype(auto) {',
    'const auto g = [&](int y) -> int {',
    'const auto g = [](int y) mutable {',
    'const auto g = []() noexcept -> bool {',
    'auto f() -> std::enable_if_t<N == 3, int> {',
    'auto f() -> decltype(a.size()) {',
    'if (ready) {',
    '} else {',
    'do {',
    'struct S {',
    'class C final {',
    'enum class E : int {',
    'union U {',
    'namespace n {',
    'template <typename T> class [[nodiscard]] R {',
    'template <typename T, int N = 4> struct A {',
    'template <typename T> struct B : public Base<T> {',
    # The rarer forms (QA on #38).
    'Foo::Foo() : a{1} {',
    '    , other{2}, last{} {',
    'Foo::Foo() : a(1), b{2} {',
    'const auto g = [&] {',
    'run([this] {',
    'return [] noexcept -> int {',
    'const auto h = [=] mutable {',
    'try {',
    'void f() try {',
    'void f() { // why',
    'if (ready) { /* why */',
    '} else { // otherwise',
    'void f() /* a comment */ {',
    'typedef struct {',
    'typedef enum : int {',
    'inline namespace v1 {',
    'export namespace n {',
    '    const noexcept {',
    '    noexcept {',
    '    override {',
    '    -> int {',
    '    requires std::integral<T> {',
    'void f() requires std::integral<T> {',
    'void f() requires (sizeof(T) > 0) {',
    'template <typename T> requires std::integral<T> struct D {',
    # Heads with parentheses in balanced groups (QA on f3d9bc0: status.hpp has these heads).
    'struct [[nodiscard("check it")]] Status {',
    'enum class [[nodiscard("why")]] StatusCode : std::uint8_t {',
    'struct alignas(32) Record {',
    'struct alignas(alignof(std::max_align_t)) Slot {',
    'class __attribute__((packed)) Header {',
    'struct Foo : Base<decltype(x)> {',
    'template <typename T> struct [[deprecated("old")]] Old : Base<decltype(T{})> {',
    "if (rate == 48'000) {",
    'if (name == "a \\") {") {',
    'if (path == "C:\\\\") {',
    "if (letter == '\\'') {",
    "if (letter == '\\\\') {",
    "if (letter == '\"') {",
    # An identifier ending in R before a string opens no raw string (which would run to the next line's ')"').
    'std::scanf("%" SCNuPTR"(", &value); if (ready) {',
    'if (text == ")") {',
)

# Allman braces, initialisers, declarations and a linkage block: none may be flagged.
ALLMAN = (
    'void f() noexcept',
    '{',
    'auto f() -> int',
    'const auto g = [&](int y) -> int',
    'extern "C" {',
    'concept Available = requires(T& value) {',
    'concept Complete = requires {',
    'constexpr bool available = requires(T& value) {',
    'return requires(T& value) {',
    'int table[] = {',
    'return {',
    'std::array values{',
    'const auto items = std::vector<int>{',
    'struct S;',
    'struct S',
    'template <typename T>',
    'class R',
    'x = y->z;',
    'registry()->entries = {',
    'lookup(key)->limits.low = {',
    'const auto items = make()->list = {',
    'const auto r = f(a, b);',
    'enum class E : int',
    'int table[] {',
    '{{1, 2}, {',
    'const std::array<int, 3> limits {',
    '    const int value',
    'struct [[nodiscard("check it")]] Status',
    'enum class [[nodiscard("why")]] StatusCode : std::uint8_t',
    'struct alignas(32) Record',
    'class __attribute__((packed)) Header',
    'struct Point origin = make_point(0, 0); {',
    # Comments and literals are not code (QA on #38: two false positives).
    '// as in f() {',
    'x = 1; // as in f() {',
    '/* if (ready) { */',
    '/* An example:',
    '   void f() {',
    '   } else {',
    '*/',
    '// a comment that a backslash continues \\',
    'void continued() {',
    'const char* text = "void f() {";',
    'const auto raw = R"(',
    'void f() {',
    'if (ready) {',
    ')";',
    'const auto delimited = u8R"x(f() { )" and on',
    'try {',
    ')x";',
    "const char brace = '{';",
    'extern "C" { // linkage',
)


def fixture_git(directory: Path, *args: str) -> None:
    environment = {name: value for name, value in os.environ.items() if name not in GIT_REPOSITORY_VARIABLES}
    subprocess.run(['git', '-C', str(directory), *args], env={**environment, **FIXTURE_GIT_SETTINGS}, capture_output=True,
                   text=True, check=True)


class BraceRuleTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        (self.root / 'common').mkdir()
        (self.root / 'common' / 'kr.cpp').write_bytes(('\n'.join(K_AND_R) + '\n').encode())
        (self.root / 'common' / 'allman.hpp').write_bytes(('\n'.join(ALLMAN) + '\n').encode())
        fixture_git(self.root, 'init', '-q')
        fixture_git(self.root, 'add', '.')

    def tearDown(self):
        self.folder.cleanup()

    def flagged(self, rev=None):
        """{file: {line numbers}} of the findings (each line counted once, whichever rule found it)."""
        lines = {}
        for finding in check_style.check_braces(self.root, rev):
            path, number = finding.split(':', 2)[:2]
            lines.setdefault(path, set()).add(int(number))
        return lines

    def test_every_k_and_r_line_is_flagged_and_no_allman_line(self):
        flagged = self.flagged()
        missed = [line for number, line in enumerate(K_AND_R, 1) if number not in flagged.get('common/kr.cpp', set())]
        self.assertEqual(missed, [], 'K&R lines the check did not flag')
        wrong = [ALLMAN[number - 1] for number in sorted(flagged.get('common/allman.hpp', set()))]
        self.assertEqual(wrong, [], 'Allman or non-brace lines the check flagged')

    def test_a_commit_is_checked_as_the_work_tree(self):
        fixture_git(self.root, 'commit', '-q', '-m', 'fixture')
        self.assertEqual(self.flagged('HEAD'), self.flagged())

    def test_the_check_reads_only_the_owned_directories(self):
        (self.root / 'third_party').mkdir()
        (self.root / 'third_party' / 'vendor.c').write_bytes(b'void f() {\n')
        fixture_git(self.root, 'add', '.')
        self.assertNotIn('third_party/vendor.c', self.flagged())


if __name__ == '__main__':
    unittest.main()
