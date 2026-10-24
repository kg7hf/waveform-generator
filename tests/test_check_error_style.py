# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-error-style.py's rules on synthetic sources, and the gate on a fixture repository.

Every old-form line below must be flagged: a const char** (char const**, with
any spacing), a const char*& named for an error (what included), a string
error text written through a non-const reference or a pointer (std::string,
std::string_view, std::wstring, std::u8string, std::u16string and
std::u32string, a string of any namespace, a std::optional of one), the names the rule lists included in snake and camel case and the whole
names what, detail(s), text, e and out, a char* or std::span<char> error
buffer, a Status written through a non-const reference or a pointer
parameter, either half of a set_error()/error() pair (a setter of an
error text, an accessor that gives one, a std::optional of a string
included), and a function with parameters, named for an error or what, that
returns a text (QA on batch 3), a member or not, its parameters named or not.
No allowed line may be: a const
char* const* input, a const char*& cursor, a const reference to an error text
or a Status (const before the type, on the line before too, or after it), a
Status by value, a local Status reference and a function returning one, a
type whose name ends in Status or string, argv, a char* or std::string& that
holds no error (names that only contain "err", "text", "out" or "e"), a
std::span<const char> input, a setter of another type, a call of a setter or
an accessor, an accessor that gives a Status or a text that is no error or
of a type whose name ends in string, a function given the failure it names
(its one parameter a Status, a StatusCode, or an error or reason enum: the
published-name tables), a string variable initialised in parentheses (from
another, from a call or from a literal, and in a block from anything, a
product or a bitwise and too), and the old form in a comment or in a string
literal, raw strings included. A function returning its failure text is
still found at namespace, class, linkage and file scope, and the other rules
still read blocks. A C status read as a truth value passes only in
tests/c_status_truth, the fixture of scripts/check-c-status-truth.py, whose
reads are wrong on purpose; the other rules still read it.
"""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'check-error-style.py'
_spec = importlib.util.spec_from_file_location('check_error_style', SCRIPT)
check_error_style = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_error_style)

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

OLD_FORMS = (
    'bool parse(const char* text, std::size_t length, const char** error) noexcept;',
    'bool validate(const Scenario& scenario, char const** error);',
    'bool fail(const char * * error, const char* message);',
    'bool reset(double rms, const char** error = nullptr) noexcept;',
    'bool configure(const Scenario& scenario, const char*& error);',
    'bool read(const std::string& path, std::string& error);',
    'bool read(const std::string& path, string& replay_error);',
    'bool load(std::string* error_text);',
    'bool open(std::string_view& message);',
    'bool check(std::string &reason);',
    'bool check(std :: string& why);',
    'bool check(std::string& failures);',
    # QA on a34f58e: the reintroductions the first rules missed.
    'bool parse(const char* text, m110::Status& status) noexcept;',
    'bool parse(const char* text, Status* error = nullptr);',
    'bool read(m110::Status *outcome);',
    'bool format(char* error, std::size_t error_size);',
    'bool format(std::span<char> error);',
    'bool format(const std::span<char, 64> message_buffer);',
    'bool read(const std::string& path, std::string& errorText);',
    'bool read(std::string& lastError);',
    'bool read(std::string& lastParseError);',
    'bool open(std::string* errMsg);',
    'bool configure(const Scenario& scenario, const char*& errorText);',
    # QA on dfcd6db and on #50's step 2: the names and types the rules missed.
    'bool read(std::string& msg);',
    'bool read(std::string& lastMsg);',
    'bool read(std::string& what);',
    'bool read(std::string& detail);',
    'bool read(std::string& details);',
    'bool read(std::string& text);',
    'bool read(std::string& e);',
    'bool read(std::string* out);',
    'bool read(std::optional<std::string>& error);',
    'bool read(std::optional<std::string_view>* reason);',
    'bool read(optional<string>& what);',
    'bool read(std::pmr::string& error);',
    'bool read(std::wstring& message);',
    'bool read(std::u8string& error);',
    'bool read(std::u16string* message);',
    'bool read(std::u32string_view& reason);',
    'bool read(other::string& why);',
    'bool configure(const char*& what);',
    'bool configure(char const*& what);',
    # The set_error()/error() pair host/capture had: either half.
    'void set_error(const char* message) noexcept;',
    'void WinmmCapture::set_error(const char* message) noexcept',
    'void set_last_error(std::string_view why);',
    'void setError(const std::string& text);',
    'void set_failure_text(std::string text);',
    'void set_message(char* text, std::size_t size);',
    '[[nodiscard]] const char* error() const noexcept',
    'const char* WasapiCapture::error() const noexcept',
    'std::string_view last_error() const;',
    'const std::string& message() const;',
    'std::string errorText();',
    'const char* what() const noexcept;',
    'char const* reason(void) const;',
    'std::optional<std::string> error() const;',
    # QA on batch 3: a function, a member or not, that returns the failure text it was asked to produce.
    'const char* parse_error(const char* text, std::size_t size);',
    'std::string failure_text(const Scenario& scenario);',
    'std::string_view validate_message(const Scenario& scenario, double rms) noexcept;',
    '[[nodiscard]] static std::string errorText(const std::string& path);',
    'const char* Capture::last_error(int index) const noexcept',
    'std::string reason(const std::filesystem::path& file)',
    'std::optional<std::string> check_error(const Options& options);',
    'const std::string& what(std::size_t index) const;',
    'char const* why(Request request);',
    'inline std::string_view diagnostic_for(const Block& block) noexcept',
    'std::string error_text(const Scenario&);',
    'std::string open_error(char* path);',
    'std::string load_failure(std::span<const std::uint8_t> bytes, std::vector<std::string> names);',
    'std::string error_text(ErrorHandler handler);',
    'std::string error_text(const ErrorHandler);',
    'std::string open_error(Request&);',
    # A digit separator is no literal: the match over it counts.
    "bool format(std::span<char, 4'096> message);",
)

ALLOWED = (
    'int main(int argc, char** argv)',
    'void list(const char* const* names, std::size_t count);',
    'bool parse_string(const char*& text, std::uint32_t& length) noexcept;',
    'void put_error(const std::string& error);',
    'void put_error(std::string const& error);',
    'void report(const\n    std::string& message);',
    'void rename(std::string& name);',
    'void copy(std::vector<std::string>& errors);',
    'm110::Status parse(const char* text, std::size_t length, Scenario& scenario) noexcept;',
    '// The old form was bool parse(const char* text, const char** error).',
    '/* bool read(const std::string& path, std::string& error); */',
    'const char* text = "bool f(const char** error)";',
    'const char* raw = R"(bool f(std::string& error))";',
    'void report(const m110::Status& status);',
    'void report(m110::Status const& status);',
    'void report(m110::Status status);',
    'm110::Status& kept = play_watch().engine_reason;',
    '[[nodiscard]] m110::Status& engine_reason() noexcept;',
    'void fill(UacStatus& uac);',
    'void log(const char* message);',
    'void copy_name(char* name, std::size_t size);',
    'void show(std::span<const char> message);',
    'void rename(std::string& terrainName);',
    'void handle(std::string& interruptText);',
    'void put_error(const std::optional<std::string>& error);',
    'void put_error(const std::pmr::string& error);',
    'void read(mystring& error);',
    'void append_field(std::string& record, std::string_view name);',
    'void split(std::string& context, std::string& texts);',
    'void read(std::string& outcome, std::string& every);',
    'void parse(std::string& text_cursor);',
    'void set_critical_error_handler(CriticalErrorHandler handler) noexcept;',
    'void set_engine_error(EngineError error, m110::Status reason) noexcept',
    'void set_name(const char* name);',
    'capture.set_error(message);',
    'capture.set_error(std::string{"a temporary"});',
    '[[nodiscard]] m110::Status status() const noexcept;',
    '[[nodiscard]] constexpr Status error() const noexcept',
    'const char* message = capture.error();',
    '[[nodiscard]] const std::string& text() const noexcept;',
    'const char* reason_name(CompletionReason reason) noexcept',
    'std::string_view error_name(StatusCode code) noexcept;',
    # A function given a failure (a Status, its code, or an error or reason enum) that names it: the
    # published-name tables (rt1170/error_map.hpp error_name()) and a tool's text for a failed Status.
    'const char* error_name(PlayerError error) noexcept',
    '[[nodiscard]] constexpr const char* error_name(EngineError error) noexcept',
    'std::string scenario_failure(m110::Status failure)',
    'std::string_view reason_text(const m110::Status& status)',
    'const char* message_of(m110::StatusCode code)',
    # A variable initialised in parentheses, and a function that returns a text named otherwise.
    'std::string message(text);',
    'std::string message("a literal");',
    'std::string error(buffer.data(), size);',
    'std::string reason(std::move(text));',
    'const std::string why(reason_text(status));',
    'const char* error = error_name(code);',
    'std::string_view text_of(const char* message)',
    'const mystring& error() const;',
    'mystring error_text(const Scenario& scenario);',
)

# Review of 4dfcf7f: in a block (a function's body, a lambda's, an
# initialiser's braces), where owned code declares no function, a string
# variable initialised in parentheses is no function returning its failure
# text, even when a product or a bitwise and there reads as a declared
# pointer or reference parameter.
BLOCK_INITIALISERS = (
    "void f()\n{\n    std::string message(count * width, ' ');\n}\n",
    'void f()\n{\n    const std::string reason(prefix & mask);\n}\n',
    "struct S\n{\n    void f() const\n    {\n        std::string error(size * count, 'x');\n    }\n};\n",
    "auto make = [](int count) { std::string message(count * width, ' '); return message; };\n",
    'namespace n\n{\nvoid f()\n{\n    if (ready)\n    {\n        std::string why(a * b, c);\n    }\n}\n}\n',
    "void f()\n{\n}\nvoid g()\n{\n    std::string message(count * width, ' ');\n}\n",
)

# A function returning its failure text is still found at namespace, class,
# linkage and file scope, after a block as before one: (source, its line).
SCOPED_RETURNS = (
    ('namespace n\n{\nstd::string error_text(const Scenario& scenario);\n}\n', 3),
    ('class C\n{\npublic:\n    std::string error_text(int code) const;\n};\n', 4),
    ('extern "C"\n{\nconst char* error_text(int code);\n}\n', 3),
    ('void f()\n{\n}\nstd::string failure_text(const Scenario& scenario)\n{\n    return {};\n}\n', 4),
    ('struct S\n{\n    void f()\n    {\n    }\n    std::string why(Request request);\n};\n', 6),
    ('template <typename T>\nclass Holder : public Base<T>\n{\n    std::string_view message(const T& value) const;\n};\n', 4),
    ('int count;\nnamespace n\n{\nstd::string error_text(const Scenario& scenario);\n}\n', 4),
    ('namespace a\n{\n}\nnamespace b\n{\nstd::string error_text(const Scenario& scenario);\n}\n', 6),
)

# Review of the C status (#50): kWFG_StatusOk is 0, so a call of a function
# that returns an enum wfg_status, left in the old bool form, still compiles
# (warn_unused_result is satisfied: the result is used) and inverts. Each
# line of C_STATUS_TRUTHS, with these declarations, reads a C status as a
# truth value; no line of C_STATUS_READS does.
C_STATUS_DECLARATIONS = (
    '__attribute__((warn_unused_result)) enum wfg_status WFG_MediaSetHost(void);\n'
    '__attribute__((warn_unused_result)) enum wfg_status WFG_MediaValidateIo(uint8_t lun, uint32_t lba, uint32_t* blocks);\n'
    '__attribute__((warn_unused_result)) enum wfg_status WFG_SD_ReadHostDetectStatus(sd_card_t* card, uint32_t* status);\n'
    '__attribute__((warn_unused_result)) enum wfg_status m110_tinyusb_stack_initialize(void);\n'
    'wfg_status wfg_usb_ep0_order_hold(const wfg_usb_setup* setup)\n{\n    return kWFG_StatusOk;\n}\n'
    'bool WFG_MediaIsLocal(void);\n'
)
C_STATUS_TRUTHS = (
    'if (!WFG_MediaSetHost())',
    'if (WFG_MediaSetHost())',
    'while (WFG_MediaSetHost())',
    'if (ready && WFG_MediaSetHost())',
    'if (WFG_MediaSetHost() || retry)',
    'const int x = WFG_MediaSetHost() ? 1 : 2;',
    'if (!WFG_MediaValidateIo(lun, lba, &blocks))',
    'if (!WFG_MediaValidateIo(lun, (lba + 1U) * 2U, &blocks))',
    'const bool host_detect_read = WFG_SD_ReadHostDetectStatus(&card, &status);',
    'bool started{WFG_MediaSetHost()};',
    'return static_cast<bool>(WFG_MediaSetHost());',
    'if (!m110_tinyusb_stack_initialize())',
    'if ( ! ::WFG_MediaSetHost ( ) )',
    'if (!wfg_usb_ep0_order_hold(&setup))',
)
C_STATUS_READS = (
    'if (WFG_MediaSetHost() != kWFG_StatusOk)',
    'if (WFG_MediaSetHost() == kWFG_StatusOk && ready)',
    'if (ready && WFG_MediaSetHost() == kWFG_StatusOk)',
    'if (kWFG_StatusOk != WFG_MediaSetHost())',
    'if (kWFG_StatusOk != WFG_MediaSetHost() && ready)',
    'if (kWFG_StatusOk == WFG_MediaSetHost() || retry)',
    'const enum wfg_status outcome = WFG_MediaSetHost();',
    'if (!status_of(WFG_MediaSetHost()).is_ok())',
    'return WFG_MediaSetHost();',
    'checks.expect(WFG_MediaSetHost() == kWFG_StatusMediaRefused && done, "refused");',
    'if (!WFG_MediaIsLocal())',
    '// if (!WFG_MediaSetHost()) was the old form',
    'const char* text = "if (!WFG_MediaSetHost())";',
    'wfg_status WFG_MediaSetHost()\n{\n    return kWFG_StatusOk;\n}',
    'enum wfg_status WFG_MediaSetHost(void)\n{\n    return kWFG_StatusOk;\n}',
    'if (!WFG_MediaSetHostLater())',
)
# The functions the repository's own headers declare to return a C status.
REPOSITORY_C_STATUS_FUNCTIONS = frozenset((
    'WFG_MediaStartInitialization', 'WFG_MediaInitialize', 'WFG_MediaSetHost', 'WFG_SD_ReadHostDetectStatus',
    'wfg_usb_ep0_order_hold', 'm110_usb_hardware_initialize', 'm110_tinyusb_stack_initialize'))

# Review of the C status (#50): GCC cannot put warn_unused_result, C's
# [[nodiscard]], on the type, so each function that returns an enum
# wfg_status carries it. Each source of C_STATUS_UNCHECKED declares one
# without it (on the line given); no source of C_STATUS_CHECKED does: a
# definition passes when a declaration of its name carries it.
C_STATUS_UNCHECKED = (
    ('enum wfg_status WFG_Foo(void);\n', 1),
    ('wfg_status wfg_cpp_entry(int x);\n', 1),
    ('static enum wfg_status read_detect(uint32_t* status)\n{\n    return kWFG_StatusOk;\n}\n', 1),
    ('__attribute__((warn_unused_result)) enum wfg_status WFG_Foo(void);\nenum wfg_status WFG_Foo(void);\n', 2),
    ('__attribute__((warn_unused_result)) enum wfg_status WFG_Foo(void);\nint count;\nenum wfg_status WFG_Bar(void);\n', 3),
    ('__attribute__((warn_unused_result)) enum wfg_status WFG_Foo(void)\n{\n}\nenum wfg_status WFG_Bar(void);\n', 4),
)
C_STATUS_CHECKED = (
    '__attribute__((warn_unused_result)) enum wfg_status WFG_Foo(void);\n',
    'static __attribute__((warn_unused_result)) enum wfg_status WFG_MediaValidateIo(uint8_t lun,\n    uint32_t lba);\n',
    '[[nodiscard]] wfg_status wfg_cpp_entry(int x);\n',
    '__attribute__((warn_unused_result))\nenum wfg_status WFG_Foo(void);\n',
    'enum wfg_status WFG_Foo(void) __attribute__((warn_unused_result));\n',
    '__attribute__((warn_unused_result)) enum wfg_status WFG_Foo(void);\nenum wfg_status WFG_Foo(void)\n{\n    return kWFG_StatusOk;\n}\n',
    'static __attribute__((warn_unused_result)) enum wfg_status read_detect(uint32_t* status)\n{\n    return kWFG_StatusOk;\n}\n',
    'const enum wfg_status outcome = WFG_Foo();\n',
)

# The other rules still read blocks: an error out-parameter of a lambda in a
# function's body.
BLOCK_OLD_FORMS = (
    ('void f()\n{\n    auto read = [](std::string& error) { error.clear(); };\n}\n', 3),
    ('void f()\n{\n    auto parse = [](const char* text, const char** error) { return text; };\n}\n', 3),
)


class RuleTests(unittest.TestCase):
    def test_every_old_form_is_flagged(self):
        for line in OLD_FORMS:
            with self.subTest(line=line):
                found = check_error_style.findings('signal-lab/src/a.cpp', line + '\n')
                self.assertEqual(len(found), 1, found)
                self.assertTrue(found[0].startswith('signal-lab/src/a.cpp:1:'), found)
                self.assertIn('<- ', found[0])

    def test_no_allowed_form_is_flagged(self):
        for line in ALLOWED:
            with self.subTest(line=line):
                self.assertEqual(check_error_style.findings('signal-lab/src/a.cpp', line + '\n'), [])

    def test_a_variable_initialised_in_a_block_is_no_function(self):
        for text in BLOCK_INITIALISERS:
            with self.subTest(text=text):
                self.assertEqual(check_error_style.findings('rt1170/a.cpp', text), [])

    def test_a_function_returning_its_failure_text_outside_a_block_is_flagged(self):
        for text, line in SCOPED_RETURNS:
            with self.subTest(text=text):
                found = check_error_style.findings('rt1170/a.cpp', text)
                self.assertEqual(len(found), 1, found)
                self.assertTrue(found[0].startswith(f'rt1170/a.cpp:{line}:'), found)
                self.assertIn('an error text a function returns', found[0])

    def test_a_c_status_read_as_a_truth_value_is_flagged(self):
        c_statuses = check_error_style.c_status_functions([C_STATUS_DECLARATIONS])
        for line in C_STATUS_TRUTHS:
            with self.subTest(line=line):
                found = check_error_style.findings('rt1170/platform/a.c', line + '\n', c_statuses)
                self.assertEqual(len(found), 1, found)
                self.assertTrue(found[0].startswith('rt1170/platform/a.c:1:'), found)
                self.assertIn('a C status read as a truth value', found[0])

    def test_a_c_status_compared_or_passed_on_is_not_flagged(self):
        c_statuses = check_error_style.c_status_functions([C_STATUS_DECLARATIONS])
        for line in C_STATUS_READS:
            with self.subTest(line=line):
                self.assertEqual(check_error_style.findings('rt1170/platform/a.c', line + '\n', c_statuses), [])

    def test_the_ast_check_s_fixture_may_read_a_c_status_as_a_truth_value(self):
        c_statuses = check_error_style.c_status_functions([C_STATUS_DECLARATIONS])
        line = 'if (!WFG_MediaSetHost())\n'
        self.assertEqual(check_error_style.findings('tests/c_status_truth/truth_forms.c', line, c_statuses), [])
        for path in ('tests/c_status_truth.c', 'tests/other/c_status_truth/truth_forms.c', 'rt1170/c_status_truth/a.c'):
            with self.subTest(path=path):
                self.assertEqual(len(check_error_style.findings(path, line, c_statuses)), 1)
        unchecked = 'enum wfg_status WFG_Foo(void);\n'
        found = check_error_style.findings('tests/c_status_truth/truth_forms.c', unchecked,
                                           check_error_style.c_status_functions([unchecked]))
        self.assertEqual(len(found), 1, found)
        self.assertIn('without __attribute__((warn_unused_result))', found[0])

    def test_the_repository_headers_declare_their_c_status_functions(self):
        headers = (SCRIPT.parents[1] / 'rt1170').rglob('*.h')
        found = check_error_style.c_status_functions(path.read_bytes().decode('utf-8') for path in headers)
        self.assertLessEqual(REPOSITORY_C_STATUS_FUNCTIONS, found.names)
        self.assertNotIn('WFG_MediaIsLocal', found.names)

    def test_a_c_status_function_without_warn_unused_result_is_flagged(self):
        for text, line in C_STATUS_UNCHECKED:
            with self.subTest(text=text):
                found = check_error_style.findings('rt1170/platform/a.c', text, check_error_style.c_status_functions([text]))
                self.assertEqual(len(found), 1, found)
                self.assertTrue(found[0].startswith(f'rt1170/platform/a.c:{line}:'), found)
                self.assertIn('without __attribute__((warn_unused_result))', found[0])

    def test_a_c_status_function_with_warn_unused_result_is_not_flagged(self):
        for text in C_STATUS_CHECKED:
            with self.subTest(text=text):
                self.assertEqual(check_error_style.findings('rt1170/platform/a.c', text, check_error_style.c_status_functions([text])), [])

    def test_the_other_rules_still_read_blocks(self):
        for text, line in BLOCK_OLD_FORMS:
            with self.subTest(text=text):
                found = check_error_style.findings('rt1170/a.cpp', text)
                self.assertEqual(len(found), 1, found)
                self.assertTrue(found[0].startswith(f'rt1170/a.cpp:{line}:'), found)

    def test_findings_name_their_line_in_order(self):
        text = '\n'.join(('// one', 'bool f(std::string& error);', 'int g();', 'bool h(const char** error);')) + '\n'
        found = check_error_style.findings('host/x.cpp', text)
        self.assertEqual([line.split(':')[1] for line in found], ['2', '4'])
        self.assertIn('return a Status or a Result', found[0])
        self.assertIn('const char**', found[1])


class GateTests(unittest.TestCase):
    """The script on a fixture repository: its exit status, and that only owned sources count."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = {key: value for key, value in os.environ.items() if key not in GIT_REPOSITORY_VARIABLES}
        self.environment.update(FIXTURE_GIT_SETTINGS)
        self.git('init', '-q')

    def git(self, *args):
        subprocess.run(['git', '-C', str(self.root), *args], check=True, capture_output=True, env=self.environment)

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode('utf-8'))
        self.git('add', path)

    def run_gate(self, *extra):
        return subprocess.run([sys.executable, str(SCRIPT), '--root', str(self.root), *extra], capture_output=True, text=True,
                              env=self.environment)

    def test_a_set_error_pair_fails(self):
        self.write('host/capture/old.hpp', 'class Capture\n{\npublic:\n    const char* error() const noexcept;\n\nprivate:\n'
                   '    void set_error(const char* message) noexcept;\n};\n')
        failed = self.run_gate()
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.assertIn('host/capture/old.hpp:4:', failed.stdout)
        self.assertIn('host/capture/old.hpp:7:', failed.stdout)
        self.assertIn('2 error out-parameters or kept or returned error texts', failed.stdout)

    def test_a_function_returning_its_failure_text_fails_and_a_name_table_passes(self):
        self.write('rt1170/error_map.hpp', 'constexpr const char* error_name(PlayerError error) noexcept\n{\n    return "x";\n}\n')
        self.write('host/render/describe.cpp', 'std::string scenario_failure(m110::Status failure);\n')
        passed = self.run_gate()
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.write('host/render/check.cpp', 'm110::Status f();\nstd::optional<std::string> check_error(const Options& options);\n')
        failed = self.run_gate()
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.assertIn('host/render/check.cpp:2:', failed.stdout)
        self.assertIn('an error text a function returns', failed.stdout)
        self.assertIn('1 error out-parameters or kept or returned error texts', failed.stdout)

    def test_a_c_status_read_as_a_truth_value_in_another_file_fails(self):
        self.write('rt1170/platform/media.h', '__attribute__((warn_unused_result)) enum wfg_status WFG_MediaSetHost(void);\n')
        self.write('rt1170/platform/media.c', 'void f(void)\n{\n    if (WFG_MediaSetHost() != kWFG_StatusOk)\n    {\n        return;\n    }\n}\n')
        passed = self.run_gate()
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertIn('0 C statuses read as a truth value', passed.stdout)
        self.write('rt1170/main.cpp', 'void g()\n{\n    if (!WFG_MediaSetHost())\n    {\n    }\n}\n')
        failed = self.run_gate()
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.assertIn('rt1170/main.cpp:3:', failed.stdout)
        self.assertIn('0 error out-parameters or kept or returned error texts, 1 C statuses read as a truth value', failed.stdout)

    def test_a_definition_passes_on_another_file_s_declaration_and_a_bare_declaration_fails(self):
        self.write('rt1170/platform/media.h', '__attribute__((warn_unused_result)) enum wfg_status WFG_MediaSetHost(void);\n')
        self.write('rt1170/platform/media.c', 'enum wfg_status WFG_MediaSetHost(void)\n{\n    return kWFG_StatusOk;\n}\n')
        self.write('tests/stand_ins.cpp', 'extern "C"\n{\nwfg_status WFG_MediaSetHost()\n{\n    return kWFG_StatusOk;\n}\n}\n')
        passed = self.run_gate()
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertIn('0 C status functions without warn_unused_result', passed.stdout)
        self.write('tests/stubs/host.h', 'enum wfg_status wfg_test_read(uint32_t* status);\n')
        failed = self.run_gate()
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.assertIn('tests/stubs/host.h:1:', failed.stdout)
        self.assertIn('0 C statuses read as a truth value, 1 C status functions without warn_unused_result', failed.stdout)

    def test_a_clean_tree_passes_and_an_old_form_fails(self):
        self.write('signal-lab/src/good.cpp', 'm110::Status f() noexcept;\n')
        self.write('third_party/lib/old.cpp', 'bool f(const char** error);\n')
        self.write('build/generated.cpp', 'bool f(const char** error);\n')
        passed = self.run_gate()
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertIn('0 error out-parameters', passed.stdout)
        self.write('rt1170/bad.hpp', 'bool f(std::string& error);\n')
        failed = self.run_gate()
        self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
        self.assertIn('rt1170/bad.hpp:1:', failed.stdout)
        self.assertIn('1 error out-parameters', failed.stdout)

    def test_rev_reads_the_commit_not_the_work_tree(self):
        self.write('tests/case.cpp', 'bool f(const char** error);\n')
        self.git('commit', '-q', '-m', 'old form')
        (self.root / 'tests/case.cpp').write_bytes(b'm110::Status f() noexcept;\n')
        self.assertEqual(self.run_gate().returncode, 0)
        committed = self.run_gate('--rev', 'HEAD')
        self.assertEqual(committed.returncode, 1, committed.stdout + committed.stderr)
        self.assertIn('tree of HEAD', committed.stdout)


if __name__ == '__main__':
    unittest.main()
