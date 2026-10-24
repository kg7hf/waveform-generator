# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""ENGINE?'s reason budget holds every message the engine can give, whole.

usb_service.cpp cuts engine_reason at engine_reason_max_chars
(rt1170/status_contract.hpp) without a mark. The reason is a Status message, a
static string: the signal-lab library's (the engine's, its stages', its
parser's), the second-source SD adapter's, or a name of rt1170/error_map.hpp.
So every string literal of signal-lab/src, signal-lab/include,
rt1170/secondary_wav.hpp and rt1170/error_map.hpp, adjacent literals
joined as the compiler joins them, must fit; a message added later is counted
when it is written. Literals that are no message (a JSON key, a format) are
counted too; none comes near the limit. The scanner reads code as
scripts/check-style.py's lexer does: comments, character literals and digit
separators open no string.
"""

import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location('check_style', ROOT / 'scripts' / 'check-style.py')
check_style = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_style)

LIMIT = re.compile(r'inline constexpr std::size_t engine_reason_max_chars = (\d+)U;')
# An escape sequence is one character of the string.
ESCAPE = re.compile(r'\\(?:x[0-9A-Fa-f]+|[0-7]{1,3}|.)', re.S)
STRING = re.compile(r'(?:u8|[uUL])?"((?:[^"\\\n]|\\.)*)"', re.S)
RAW_STRING = re.compile(r'(?:u8|[uUL])?R"([^()\\\s"]{0,16})\((.*)\)\1"', re.S)
CHARACTER = re.compile(r"(?:u8|[uUL])?'")


def engine_reason_max_chars() -> int:
    found = LIMIT.search((ROOT / 'rt1170' / 'status_contract.hpp').read_text(encoding='utf-8'))
    if found is None:
        raise AssertionError('rt1170/status_contract.hpp no longer defines engine_reason_max_chars as this test reads it')
    return int(found.group(1))


def string_literals(text: str) -> list[str]:
    """The text of each string literal in `text`, in order; literals with only white space between them are one."""
    literals = []
    previous_end = None
    for match in check_style.LEXEME.finditer(text):
        token = match.group()
        if match.group('blank') is None or token.startswith(('//', '/*')) or CHARACTER.match(token):
            continue
        raw = RAW_STRING.fullmatch(token)
        plain = STRING.fullmatch(token)
        if raw is None and plain is None:
            continue
        value = raw.group(2) if raw is not None else ESCAPE.sub('?', plain.group(1))
        if previous_end is not None and text[previous_end:match.start()].strip() == '' and literals:
            literals[-1] += value
        else:
            literals.append(value)
        previous_end = match.end()
    return literals


def message_sources() -> list[Path]:
    sources = [path for folder in ('signal-lab/src', 'signal-lab/include') for path in sorted((ROOT / folder).rglob('*'))
               if path.suffix in ('.cpp', '.hpp', '.h')]
    return [*sources, ROOT / 'rt1170' / 'error_map.hpp', ROOT / 'rt1170' / 'secondary_wav.hpp']


class ScannerTests(unittest.TestCase):
    def test_joined_literals_count_whole_and_no_comment_or_character_opens_one(self):
        text = '\n'.join((
            '// a "quoted" word in a comment',
            "if (c == '\"' || rate == 48'000) /* \"also\" */",
            '    return fail("design: the spread must be "',
            '                "finite", "two");',
            'const char* raw = R"x(a "raw" one)x";',
            'const char* escaped = "tab\\there \\x7F";',
        ))
        self.assertEqual(string_literals(text), ['design: the spread must be finite', 'two', 'a "raw" one', 'tab?here ?'])

    def test_the_sources_are_read(self):
        literals = [literal for path in message_sources() for literal in string_literals(path.read_text(encoding='utf-8'))]
        self.assertIn('engine: output clipped with clip_policy reject', literals)
        self.assertIn('usb_unconfirmed', literals)
        self.assertIn('second_source: SD WAV read failed or was short', literals)


class BudgetTests(unittest.TestCase):
    def test_every_message_fits_the_engine_reason(self):
        limit = engine_reason_max_chars()
        too_long = []
        for path in message_sources():
            for literal in string_literals(path.read_text(encoding='utf-8')):
                if len(literal) > limit:
                    too_long.append(f'{path.relative_to(ROOT).as_posix()}: {len(literal)} characters: {literal}')
        self.assertEqual(too_long, [], f'messages longer than engine_reason_max_chars ({limit}): ENGINE? would cut them')


class LiteralFixtureTests(unittest.TestCase):
    def test_a_long_message_in_a_source_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'stage.cpp'
            source.write_bytes(('constexpr const char* message = "' + 'x' * 129 + '";\n').encode('utf-8'))
            self.assertEqual([len(literal) for literal in string_literals(source.read_text(encoding='utf-8'))], [129])


if __name__ == '__main__':
    unittest.main()
