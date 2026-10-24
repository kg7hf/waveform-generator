#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check the two formatting rules any machine can check: Allman braces and LF endings.

The repository's style is Allman (docs/coding-standard.md). clang-format owns
the full C++ layout; this lightweight gate also covers C and tracked text:

- Allman braces in the owned C and C++ sources: no line ends in an opening
  brace after ")", "else", "do" or "try" (a function, control statement or
  lambda in K&R style), also when a function head's qualifiers (const,
  volatile, noexcept with or without its condition, override, final, a
  lambda's mutable, & and &&), a trailing return type ("-> T", lambdas
  included) or a requires clause stand between the ")" and the brace, or stand
  alone on a continuation line before it; none after a lambda introducer
  without a parameter list ("[&] {"), nor after a constructor's member
  initialiser ("a{1} {"); and no namespace, class, struct, enum or union head
  ends in one, a template head, typedef, inline or export before it included,
  nor one with an attribute's arguments, an alignas(), an __attribute__(())
  or a decltype() in it.
  Initialiser braces ("= {", "return {") are not braces of this kind, nor is a
  linkage block ('extern "C" {', which C headers open for C++). Comments and
  string and character literals are not code: their text is blanked before the
  rules read a line, so "{ // why" is still a K&R brace, "void f() /* c */ {"
  too, and "// as in f() {" or a raw string's "f() {" line is none.
- LF line endings: no tracked text file holds a carriage return, except the
  files .gitattributes marks -text (the pathsim patches, kept byte for byte).

Both read Git's view of the files, binary files skipped: the tracked files of
the work tree by default, or the tree of a commit with --rev, which leaves
uncommitted edits out. Run from any directory; the host CI runs it on every
push. Exit status 0 means both rules hold.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

OWNED_SOURCE_DIRECTORIES = ('channel-sim', 'common', 'examples', 'host', 'rt1170', 'rt1064', 'signal-lab', 'tests',
                            'waveform-source')
SOURCE_EXTENSIONS = ('c', 'cc', 'cpp', 'cxx', 'h', 'hh', 'hpp', 'inl', 'ipp')

# The lexemes that matter for reading a line as code, in the order the scanner
# tries them at each position. Comments and literals ("blank") are blanked, line
# breaks kept; numbers and identifiers ("keep") are consumed whole, so a match
# never starts inside one: "FOOR" followed by a quote is no raw string, and a
# digit separator (48'000) opens no character literal. An unterminated string
# or character literal ends at its line, an unterminated block comment at the
# end of the file.
LEXEME = re.compile(r'''
    (?P<blank>
        //(?:[^\\\n]|\\.)*                                          # line comment (a backslash-newline continues it)
      | /\*.*?(?:\*/|\Z)                                            # block comment
      | (?:u8|[uUL])?R"(?P<delimiter>[^()\\\s"]{0,16})\(.*?\)(?P=delimiter)"   # raw string literal
      | (?:u8|[uUL])?"(?:[^"\\\n]|\\.)*"?                           # string literal
      | (?:u8|[uUL])?'(?:[^'\\\n]|\\.)*'?                           # character literal
    )
  | (?P<keep>
        \.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.]|'(?=[0-9A-Za-z_]))*    # number, digit separators included
      | [A-Za-z_][A-Za-z0-9_]*                                      # identifier or keyword
    )
''', re.S | re.X)
NOT_LINE_BREAK = re.compile(r'[^\n]')

# What may stand between a function head's ")" and its body: qualifiers, noexcept
# with one level of parentheses for its condition, a trailing return type and a
# requires clause. A type holds no assignment ("==", "!=", "<=" and ">=" may
# stand in a template argument), so "f()->member = {", an initialiser, is not
# taken for one.
QUALIFIER = r'(?:(?:const|volatile|mutable|override|final)\b|noexcept\b(?:\s*\([^()]*\))?|&&?)'
FUNCTION_QUALIFIERS = rf'(?:\s*{QUALIFIER})*'
TRAILING_RETURN = r'(?:\s*->(?:[^{};=]|[=!<>]=)*)?'
REQUIRES_CLAUSE = r'(?:\s*\brequires\b[^{};]*)?'
OPENING_BRACE = r'\s*\{\s*$'
# A type head's text up to its brace: no ";" or "=" (a declaration or an
# initialiser), and parentheses only in balanced groups, three levels deep: an
# attribute with arguments ([[nodiscard("why")]]), alignas(alignof(T)),
# __attribute__((packed)) or a decltype(...) in a base class.
PARENTHESISED = r'\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)'
# A requires expression after an assignment or return is not a function body.
# clang-format's Allman profile keeps its opening brace on the same line.
# Do not exempt a function's trailing requires clause (including parenthesized constraints).
REQUIRES_EXPRESSION = re.compile(rf'(?:=|\breturn)\s*requires\s*(?:{PARENTHESISED})?{OPENING_BRACE}')
HEAD_TEXT = rf'(?:[^;=()]|{PARENTHESISED})*'
# One pattern per K&R form, each expected to match no line of code.
BRACE_RULES = tuple((description, re.compile(pattern)) for description, pattern in (
    ('opening brace after ")" (with any qualifiers, trailing return type or requires clause), "else", "do" or "try"',
     rf'(?:\){FUNCTION_QUALIFIERS}{TRAILING_RETURN}{REQUIRES_CLAUSE}|\b(?:else|do|try)){OPENING_BRACE}'),
    ('opening brace after a qualifier, trailing return type or requires clause alone on a continuation line',
     rf'^(?=\s*(?:(?:const|volatile|mutable|override|final|noexcept|requires)\b|->))'
     rf'{FUNCTION_QUALIFIERS}{TRAILING_RETURN}{REQUIRES_CLAUSE}{OPENING_BRACE}'),
    ('opening brace after a lambda introducer without a parameter list',
     rf'(?:^|[=(,{{]|\breturn)\s*\[[^\[\]]*\]{FUNCTION_QUALIFIERS}{TRAILING_RETURN}{OPENING_BRACE}'),
    ("opening brace after a constructor's member initialiser in braces",
     rf'\}}{OPENING_BRACE}'),
    ('opening brace on a namespace, class, struct, enum or union head',
     r'^\s*(?:(?:typedef|inline|export)\s+)*(?:template\s*<.*>\s*)?(?:namespace|class|struct|enum|union)\b' + HEAD_TEXT
     + OPENING_BRACE),
))
CARRIAGE_RETURN = r'\r'


def git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True, encoding='utf-8',
                          errors='replace')


def grep(root: Path, rev: str | None, options: list[str], pattern: str, pathspecs: list[str]) -> list[str]:
    """Lines (or names, with -l) that match; git grep exits 1 when nothing matches."""
    result = git(root, 'grep', '-I', '-P', *options, '-e', pattern, *([rev] if rev else []), '--', *pathspecs)
    if result.returncode not in (0, 1):
        raise RuntimeError(f'git grep failed: {result.stderr.strip()}')
    prefix = f'{rev}:' if rev else ''
    return [line.removeprefix(prefix) for line in result.stdout.splitlines()]


def is_owned_source(path: str) -> bool:
    top, _, rest = path.partition('/')
    return bool(rest) and top in OWNED_SOURCE_DIRECTORIES and path.rpartition('.')[2] in SOURCE_EXTENSIONS


def owned_sources(root: Path, rev: str | None) -> list[str]:
    """The tracked owned C and C++ sources: of the work tree, or of the commit's tree."""
    listed = git(root, 'ls-tree', '-r', '-z', '--name-only', rev) if rev else git(root, 'ls-files', '-z')
    if listed.returncode:
        raise RuntimeError(f'listing the tracked files failed: {listed.stderr.strip()}')
    return sorted(path for path in listed.stdout.split('\0') if path and is_owned_source(path))


def read_sources(root: Path, rev: str | None, paths: list[str]) -> dict[str, bytes]:
    """Each source's bytes: the work tree's file (one deleted there is left out), or the commit's blob."""
    if not rev:
        return {path: (root / path).read_bytes() for path in paths if (root / path).is_file()}
    request = ''.join(f'{rev}:{path}\n' for path in paths).encode('utf-8')
    batch = subprocess.run(['git', '-C', str(root), 'cat-file', '--batch'], input=request, capture_output=True)
    if batch.returncode:
        raise RuntimeError(f'git cat-file failed: {batch.stderr.decode(errors="replace").strip()}')
    contents, position, output = {}, 0, batch.stdout
    for path in paths:
        end = output.index(b'\n', position)
        header = output[position:end].split()
        if len(header) != 3 or header[1] != b'blob':
            raise RuntimeError(f'git cat-file has no blob for {rev}:{path}')
        size = int(header[2])
        contents[path] = output[end + 1:end + 1 + size]
        position = end + 1 + size + 1
    return contents


def code_only(text: str) -> str:
    """The text with every comment and string or character literal blanked, its line breaks kept."""
    return LEXEME.sub(lambda match: NOT_LINE_BREAK.sub(' ', match.group()) if match.group('blank') is not None
                      else match.group(), text)


def brace_findings(path: str, text: str) -> list[str]:
    findings = []
    for number, (line, code) in enumerate(zip(text.split('\n'), code_only(text).split('\n')), 1):
        if REQUIRES_EXPRESSION.search(code):
            continue
        description = next((description for description, rule in BRACE_RULES if rule.search(code)), None)
        if description is not None:
            findings.append(f'{path}:{number}:{line.rstrip()}  <- K&R brace: {description}')
    return findings


def check_braces(root: Path, rev: str | None) -> list[str]:
    findings = []
    for path, data in read_sources(root, rev, owned_sources(root, rev)).items():
        if b'\0' not in data:  # a binary file is no source
            findings += brace_findings(path, data.decode('utf-8', errors='replace'))
    return findings


def check_line_endings(root: Path, rev: str | None) -> list[str]:
    exempt = unset_text_paths(root)
    names = grep(root, rev, ['-l'], CARRIAGE_RETURN, ['.'])
    return [f'{name}  <- carriage return in a text file (CRLF or CR line ending)' for name in names
            if name not in exempt]


def unset_text_paths(root: Path) -> set[str]:
    """Tracked paths whose text attribute is unset (-text): their bytes are kept as committed."""
    listed = git(root, 'ls-files', '-z')
    if listed.returncode:
        raise RuntimeError(f'git ls-files failed: {listed.stderr.strip()}')
    attributes = subprocess.run(['git', '-C', str(root), 'check-attr', '-z', '--stdin', 'text'], input=listed.stdout,
                                capture_output=True, text=True, encoding='utf-8')
    if attributes.returncode:
        raise RuntimeError(f'git check-attr failed: {attributes.stderr.strip()}')
    fields = attributes.stdout.split('\0')
    return {fields[index] for index in range(0, len(fields) - 2, 3) if fields[index + 2] == 'unset'}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--rev', help='Check this commit\'s tree (for example HEAD) instead of the work tree')
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        braces = check_braces(root, args.rev)
        endings = check_line_endings(root, args.rev)
    except (OSError, RuntimeError, ValueError) as error:
        print(f'Style check failed to run: {error}', file=sys.stderr)
        return 2
    for finding in braces + endings:
        print(finding)
    where = f'tree of {args.rev}' if args.rev else 'work tree'
    print(f'Style ({where}): {len(braces)} K&R brace lines in owned C/C++ sources, '
          f'{len(endings)} text files with a carriage return.')
    return 1 if braces or endings else 0


if __name__ == '__main__':
    sys.exit(main())
