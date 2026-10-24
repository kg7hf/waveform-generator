#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Gate on the owned functions that return bool: none of them is fallible (#50).

A fallible function returns m110::Status or m110::Result<T>, and owned C code
an enum wfg_status (rt1170/platform/wfg_status.h); bool is for a question
that cannot fail. No pattern can tell the two apart, so
scripts/bool-returns-baseline.json lists every bool-returning function of the
owned non-test code under one of three classes:

- "fallible": an operation that can fail and reports the failure as false (a
  read, a write, a parse, an open, a refused request). #50 took this list to
  zero, and the check fails on any entry in it, so it stays empty;
- "predicates": a function whose false is an answer, never a failure: a
  question (is_*, has_*, empty, a comparison operator, a range or table check)
  or a poll whose false means "nothing yet" (a reply not yet there). Each says
  why in a few words;
- "abi": a callback whose bool an outside C interface fixes (TinyUSB, the NXP
  SDK), so it stays bool whatever false means there. Each names the interface.

The rules:

- a bool-returning function the baseline does not list fails: return a Status
  or a Result, or list it under "predicates" (or "abi") with why in the same
  commit;
- the fallible list stays empty: any entry in it fails, whatever the code,
  so a fallible function cannot be listed by hand, nor a predicate moved to
  it;
- an entry counts the bool-returning declarations of its name in its file
  (overloads, and a declaration before a definition); finding more fails, so a
  second overload of a listed name gets no free pass;
- an entry whose function is gone, or that finds fewer declarations than it
  counts, fails until --update drops or lowers it, so a function brought back
  later with bool finds no stale entry to hide behind;
- against the baseline of the previous revision (--since; by default HEAD for
  the work tree, the commit's first parent with --rev), a fallible entry moved
  to "predicates" or "abi", and a predicate or abi entry that counts more
  declarations, fail unless the baseline's "reclassified" list names it with
  why and at least that count. Otherwise a fallible function could be admitted
  under a predicate's name, or the fallible list emptied by reclassifying its
  entries, with the code unchanged; the list makes each such change show in
  review. A reclassified entry names a predicate or abi entry and stays while
  it does; --update drops it with its entry, and leaves the list out when it
  is empty. Where that revision or its baseline cannot be read (the commit
  that adds the gate, a shallow clone), the comparison is skipped and the
  summary says so;
- --update rewrites the baseline only when the check passes apart from gone
  functions and fallen counts, which it drops and lowers. It adds no entry:
  a predicate is listed by hand with its reason.

What counts as returning bool is read from the code with comments, string and
character literals (check-style.py's lexer) and preprocessor directives
blanked: a function declared or defined at namespace or class scope with the
return type bool, operators and "operator bool" included; a function with a
trailing "-> bool", lambdas in function bodies included (named by the variable
that holds them); and a function type with a bool result: a function pointer
or reference (a member, a parameter, a using or typedef alias), a parameter of
function type, or a template argument (std::function<bool(...)>). A function
whose return type is deduced ("auto" or "decltype(auto)" before its name, a
lambda without "->") is found when its own body has "return true;" or "return
false;" (the line of its head), as signal_lab_render's queue_live_events was;
one that returns only another bool expression is not.

The sources are the tracked owned C and C++ files below the complexity
ratchet's measured paths and the Watterson validation driver
(signal-lab/validation), but not a test: a file named tests.cpp, *_tests.cpp,
*_test.cpp or test_*. --rev reads a commit's tree and that tree's baseline in
place of the work tree, so each commit is checked as it was made; --baseline
names another file, with --rev too. --list prints every declaration found
with its class.
Exit status 0: the check passes; 1: failures, one per line; 2: the check could
not run.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import sys


def _load(name: str, file_name: str):
    """A sibling script as a module, under a name of its own."""
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().with_name(file_name))
    module = importlib.util.module_from_spec(spec)
    # Its dataclasses look their module up while the class is made.
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


check_style = _load('wfg_bool_returns_check_style', 'check-style.py')
check_complexity = _load('wfg_bool_returns_check_complexity', 'check-complexity.py')

GATED_PATHS = (*check_complexity.MEASURED_PATHS, 'signal-lab/validation')
TEST_SOURCE = re.compile(r'^(?:tests?\.\w+|\w*_tests?\.\w+|test_\w*\.\w+)$')
BASELINE = PurePosixPath('scripts/bool-returns-baseline.json')
FALLIBLE = 'fallible'
PREDICATES = 'predicates'
ABI = 'abi'
KINDS = (FALLIBLE, PREDICATES, ABI)
# The baseline's optional fourth list: the predicate and abi entries this or an
# earlier revision moved from "fallible" or counted more of, each with why.
RECLASSIFIED = 'reclassified'
ENTRY_FIELDS = frozenset(('file', 'function', 'count', 'why'))

# A preprocessor directive, its backslash-continued lines included.
DIRECTIVE = re.compile(r'^[ \t]*#(?:[^\n\\]|\\(?:\n|.))*', re.M)
NOT_LINE_BREAK = re.compile(r'[^\n]')
TOKEN = re.compile(r'(?P<word>[A-Za-z_]\w*)|(?P<number>\.?\d(?:[eEpP][+-]|[\w.]|\'(?=\w))*)|(?P<punct>::|->|[^\s\w])')
# Words before which a "(" opens no function's parameter list.
NOT_FUNCTION_NAMES = frozenset(('if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'alignof', 'alignas',
                                'decltype', 'noexcept', 'requires', 'static_assert', '__attribute__', 'operator'))
# What may stand between a parameter list's ")" and a trailing "->".
TRAILING_QUALIFIERS = frozenset(('const', 'volatile', 'noexcept', 'mutable', 'constexpr', 'consteval', 'static',
                                 'override', 'final', '&'))
# A "bool name(...)" whose "(" holds one of these first initialises a variable.
INITIALISER_STARTS = frozenset(('true', 'false', 'nullptr', '!', '-', '+', '~'))
DECLARATIVE_SCOPES = frozenset(('file', 'namespace', 'class', 'linkage'))


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int


@dataclass(frozen=True)
class Found:
    """One bool-returning declaration: its file, its qualified name and the line of its "bool"."""
    file: str
    name: str
    line: int

    @property
    def key(self) -> tuple[str, str]:
        return self.file, self.name


@dataclass
class Scope:
    kind: str  # file, namespace, class, linkage (extern "C"), body (a function's or a block's) or init
    name: str | None
    statement_start: int = 0
    parens: list[int] = field(default_factory=list)
    # A function's or a lambda's body (not a block's), and, while its return type is deduced and no
    # "return true;" or "return false;" has made it bool yet, its declaration.
    function: bool = False
    deduced: Found | None = None


def code_only(text: str) -> str:
    """check-style.py's code, comments and literals blanked, with the preprocessor directives blanked too."""
    return DIRECTIVE.sub(lambda match: NOT_LINE_BREAK.sub(' ', match.group()), check_style.code_only(text))


def tokens_of(code: str) -> list[Token]:
    result, line, position = [], 1, 0
    for match in TOKEN.finditer(code):
        line += code.count('\n', position, match.start())
        position = match.start()
        result.append(Token(match.lastgroup, match.group(), line))
    return result


def skip_group(texts: list[str], position: int, opening: str, closing: str) -> int:
    """The position after the balanced group that opens at `position`."""
    depth = 0
    while position < len(texts):
        depth += (texts[position] == opening) - (texts[position] == closing)
        position += 1
        if depth == 0:
            break
    return position


def without_attributes(texts: list[str]) -> list[str]:
    """A head's token texts without its attributes: [[...]], alignas(...), __attribute__((...)), __declspec(...)."""
    result, position = [], 0
    while position < len(texts):
        text = texts[position]
        following = texts[position + 1] if position + 1 < len(texts) else None
        if text == '[' and following == '[':
            position = skip_group(texts, position, '[', ']')
        elif text in ('alignas', '__attribute__', '__declspec') and following == '(':
            position = skip_group(texts, position + 1, '(', ')')
        else:
            result.append(text)
            position += 1
    return result


def classify(head: list[Token]) -> Scope:
    """The scope a "{" opens, from the tokens of its statement before it."""
    texts = without_attributes([token.text for token in head])
    start = 0
    while start < len(texts) and texts[start] in ('inline', 'export'):
        start += 1
    if start < len(texts) and texts[start] == 'namespace':
        name = ''.join(texts[start + 1:])
        return Scope('namespace', name if re.fullmatch(r'\w+(?:::\w+)*', name) else None)
    if texts == ['extern']:  # extern "C" {, the string blanked
        return Scope('linkage', None)
    if texts[:1] == ['template']:
        texts = texts[skip_group(texts, 1, '<', '>'):]
    for position, text in enumerate(texts):
        if text in ('(', '='):
            break
        if text == 'enum':
            return Scope('init', None)
        if text in ('class', 'struct', 'union'):
            rest = [word for word in texts[position + 1:] if word != 'final']
            if not rest or rest[0] == ':':
                return Scope('class', None)
            if len(rest) == 1 or rest[1] == ':':
                return Scope('class', rest[0] if re.fullmatch(r'\w+', rest[0]) else None)
            break
    depth = 0
    for text in texts:
        depth += (text in ('(', '[', '<')) - (text in (')', ']', '>'))
        if text == '=' and depth == 0:
            return Scope('init', None)
    return Scope('body', function_name(head))


def function_name(head: list[Token]) -> str | None:
    """The name of the function a body's head defines: the qualified name before its first "(" outside brackets."""
    depth = 0
    for position, token in enumerate(head):
        depth += (token.text == '[') - (token.text == ']')
        if token.text != '(' or depth:
            continue
        if position == 0 or head[position - 1].kind != 'word' or head[position - 1].text in NOT_FUNCTION_NAMES:
            return None
        parts, back = [head[position - 1].text], position - 1
        while back >= 2 and head[back - 1].text == '::' and head[back - 2].kind == 'word':
            back -= 2
            parts.insert(0, head[back].text)
        return '::'.join(parts)
    return None


class Scanner:
    """Walks one source's tokens with a stack of brace scopes and records each bool-returning declaration."""

    def __init__(self, path: str, tokens: list[Token]):
        self.path = path
        self.tokens = tokens
        self.scopes = [Scope('file', None)]
        self.found: list[Found] = []

    def text(self, index: int) -> str | None:
        return self.tokens[index].text if 0 <= index < len(self.tokens) else None

    def is_word(self, index: int) -> bool:
        return 0 <= index < len(self.tokens) and self.tokens[index].kind == 'word'

    def match_forward(self, index: int, opening: str, closing: str) -> int:
        """The index of the token that closes the one at `index`, or len(tokens)."""
        depth = 0
        for position in range(index, len(self.tokens)):
            depth += (self.tokens[position].text == opening) - (self.tokens[position].text == closing)
            if depth == 0:
                return position
        return len(self.tokens)

    def match_backward(self, index: int, opening: str, closing: str) -> int:
        """The index of the token that opens the one at `index`, or -1."""
        depth = 0
        for position in range(index, -1, -1):
            depth += (self.tokens[position].text == closing) - (self.tokens[position].text == opening)
            if depth == 0:
                return position
        return -1

    def qualified_before(self, index: int) -> str | None:
        """The qualified name (a::b::c, or a destructor's ~c) that ends just before `index`."""
        position = index - 1
        if not self.is_word(position):
            return None
        parts = [self.tokens[position].text]
        while self.text(position - 1) == '::' and self.is_word(position - 2):
            position -= 2
            parts.insert(0, self.tokens[position].text)
        if self.text(position - 1) == '~':
            parts[0] = '~' + parts[0]
        return '::'.join(parts)

    def prefix(self) -> list[str]:
        """The names of the enclosing namespaces and classes, and of the outermost enclosing function."""
        names = []
        for scope in self.scopes:
            if scope.kind in ('namespace', 'class', 'body') and scope.name:
                names.append(scope.name)
            if scope.kind == 'body':
                break
        return names

    def record(self, index: int, name: str) -> None:
        self.found.append(Found(self.path, '::'.join([*self.prefix(), name]), self.tokens[index].line))

    def in_parameters(self, name: str) -> str:
        """`name` qualified by the function whose parameter list it stands in, if it stands in one."""
        scope = self.scopes[-1]
        if not scope.parens:
            return name
        function = self.qualified_before(scope.parens[0])
        return f'{function}::{name}' if function else name

    def run(self) -> list[Found]:
        for index, token in enumerate(self.tokens):
            scope = self.scopes[-1]
            text = token.text
            if text == '{':
                opened = classify(self.tokens[scope.statement_start:index])
                opened.statement_start = index + 1
                opened.function, opened.deduced = self.body_at(index, scope)
                self.scopes.append(opened)
            elif text == 'return' and self.text(index + 1) in ('true', 'false') and self.text(index + 2) == ';':
                self.literal_return()
            elif text == '}':
                if len(self.scopes) > 1:
                    self.scopes.pop()
                if not self.scopes[-1].parens:
                    self.scopes[-1].statement_start = index + 1
            elif text == ';' and not scope.parens:
                scope.statement_start = index + 1
            elif text == '(':
                scope.parens.append(index)
            elif text == ')' and scope.parens:
                scope.parens.pop()
            elif text == 'bool' and scope.kind in DECLARATIVE_SCOPES:
                self.bool_at(index, scope)
            elif text == '->' and self.text(index + 1) == 'bool':
                self.trailing_return(index)
        # A deduced return is found at its body's first "return true|false;", after what its body declares.
        return sorted(self.found, key=lambda found: found.line)

    def literal_return(self) -> None:
        """A "return true;" or "return false;": its function or lambda returns bool if its return type is deduced."""
        function = next((scope for scope in reversed(self.scopes) if scope.function), None)
        if function is not None and function.deduced is not None:
            self.found.append(function.deduced)
            function.deduced = None

    def before_specifiers(self, position: int) -> int:
        """The index of the token before the specifiers that end at `position` (const, noexcept(...), mutable ...)."""
        while position >= 0:
            text = self.tokens[position].text
            if text in TRAILING_QUALIFIERS:
                position -= 1
                continue
            if text == ')' and self.text(self.match_backward(position, '(', ')') - 1) == 'noexcept':
                position = self.match_backward(position, '(', ')') - 2
                continue
            return position
        return position

    def lambda_introducer(self, close: int) -> int | None:
        """The index of the "[" whose "]" is at `close`, when it introduces a lambda rather than a subscript."""
        introducer = self.match_backward(close, '[', ']')
        before = self.text(introducer - 1)
        subscript = before in (')', ']') or (self.is_word(introducer - 1) and before != 'return')
        return None if introducer < 0 or subscript else introducer

    def body_at(self, brace: int, outer: Scope) -> tuple[bool, Found | None]:
        """Whether the "{" at `brace` opens a function's or a lambda's body, and its declaration when its return
        type is deduced: a lambda without "->", or "auto" (or "decltype(auto)") before the function's name."""
        position = self.before_specifiers(brace - 1)
        text = self.text(position)
        if text == ']':  # a lambda without parameters: [&] { ... }
            introducer = self.lambda_introducer(position)
            return (True, self.deduced_lambda(introducer)) if introducer is not None else (False, None)
        if text != ')':
            return self.has_trailing_return(position), None
        opening = self.match_backward(position, '(', ')')
        introducer = self.lambda_introducer(opening - 1) if self.text(opening - 1) == ']' else None
        if introducer is not None:
            return True, self.deduced_lambda(introducer)
        name = self.function_name_before(opening)
        if name is None or name in NOT_FUNCTION_NAMES:
            return False, None
        head = [token.text for token in self.tokens[outer.statement_start:opening]]
        if 'auto' not in head:
            return True, None
        line = self.tokens[opening - 1].line
        return True, Found(self.path, '::'.join([*self.prefix(), name]), line)

    def function_name_before(self, opening: int) -> str | None:
        """The name of the function whose parameters open at `opening`: a qualified name, or an operator's
        (S::operator[], operator(), operator bool)."""
        for back in range(1, 4):
            if self.text(opening - back) == 'operator':
                symbol = ''.join(token.text for token in self.tokens[opening - back + 1:opening])
                spacing = ' ' if symbol[:1].isalpha() else ''
                return f'{self.qualified_before(opening - back + 1)}{spacing}{symbol}'
        return self.qualified_before(opening)

    def has_trailing_return(self, position: int) -> bool:
        """Whether the tokens that end at `position` are a trailing return type: ") [specifiers] -> type"."""
        depth = 0
        while position >= 0 and (depth or self.tokens[position].text not in (';', '{', '}')):
            text = self.tokens[position].text
            depth += (text == ')') - (text == '(')
            if text == '->' and not depth:
                return self.text(self.before_specifiers(position - 1)) == ')'
            position -= 1
        return False

    def deduced_lambda(self, introducer: int) -> Found:
        holder = self.text(introducer - 2) if self.text(introducer - 1) == '=' and self.is_word(introducer - 2) else None
        name = f'<lambda {holder}>' if holder else '<lambda>'
        return Found(self.path, '::'.join([*self.prefix(), name]), self.tokens[introducer].line)

    def bool_at(self, index: int, scope: Scope) -> None:
        if self.text(index - 1) == 'operator':
            if not scope.parens and self.text(index + 1) == '(':
                # Defined outside its class: Status::operator bool.
                qualifier = self.qualified_before(index - 2) if self.text(index - 2) == '::' else None
                self.record(index, f'{qualifier}::operator bool' if qualifier else 'operator bool')
            return
        if self.text(index + 1) == '(':
            self.function_type(index)
            return
        # At namespace or class scope "bool name(" declares a function, unless the
        # parentheses initialise a variable; in a parameter list, a parameter of
        # function type (void visit(bool keep(int))).
        name, after = self.declarator(index + 1)
        if self.text(after) == '<':  # an explicit specialisation: template <> bool f<int>(int)
            after = self.match_forward(after, '<', '>') + 1
        if name is None or self.text(after) != '(':
            return
        first = after + 1
        if self.text(first) in INITIALISER_STARTS or (first < len(self.tokens) and self.tokens[first].kind == 'number'):
            return
        self.record(index, self.in_parameters(name))

    def declarator(self, index: int) -> tuple[str | None, int]:
        """The qualified name of a function declarator from `index` (operators included), and the index after it."""
        parts, position = [], index
        while True:
            if self.text(position) == '~' and self.is_word(position + 1):
                parts.append('~' + self.tokens[position + 1].text)
                position += 2
            elif self.text(position) == 'operator':
                return self.operator_name(parts, position + 1)
            elif self.is_word(position):
                parts.append(self.tokens[position].text)
                position += 1
            else:
                return None, position
            if self.text(position) != '::':
                return '::'.join(parts), position
            position += 1

    def operator_name(self, parts: list[str], position: int) -> tuple[str, int]:
        """operator==, operator(), operator[]: the name, and the index of the "(" of its parameters."""
        if self.text(position) in ('(', '[') and self.text(position + 1) in (')', ']'):
            symbol, position = self.tokens[position].text + self.tokens[position + 1].text, position + 2
        else:
            symbol = ''
            while position < len(self.tokens) and self.tokens[position].text != '(':
                symbol += self.tokens[position].text
                position += 1
        return '::'.join([*parts, 'operator' + symbol]), position

    def function_type(self, index: int) -> None:
        """bool followed by "(": a function pointer or reference declarator, or a function type."""
        position = index + 2
        while self.is_word(position) and self.text(position + 1) == '::':  # a member pointer: bool (Class::*name)(...)
            position += 2
        if self.text(position) in ('*', '&'):
            position += 1
            while self.text(position) in ('const', 'volatile'):
                position += 1
            name = None
            if self.is_word(position):
                name = self.tokens[position].text
                position += 1
            if self.text(position) == ')' and self.text(position + 1) == '(':
                name = name or self.alias_name(index)
                if name is not None:
                    self.record(index, self.in_parameters(name))
            return
        name = self.alias_name(index)
        if name is None and self.text(index - 1) == '<':
            name = self.templated_name(index - 1)
        if name is not None:
            self.record(index, name)

    def alias_name(self, index: int) -> str | None:
        """The name a "using NAME = bool ..." or "typedef bool ... NAME" alias gives the function type at `index`."""
        if self.text(index - 1) == '=' and self.is_word(index - 2) and self.text(index - 3) == 'using':
            return self.tokens[index - 2].text
        if self.text(index - 1) == 'typedef':
            close = self.match_forward(index + 1, '(', ')')
            inner = [token.text for token in self.tokens[index + 2:close]
                     if token.kind == 'word' and token.text not in ('const', 'volatile')]
            return inner[-1] if inner else None
        return None

    def templated_name(self, less: int) -> str | None:
        """The name declared with the template whose arguments open at `less`: std::function<bool(int)> name."""
        position = self.match_forward(less, '<', '>') + 1
        while self.text(position) in ('&', '*', 'const'):
            position += 1
        return self.in_parameters(self.tokens[position].text) if self.is_word(position) else None

    def trailing_return(self, index: int) -> None:
        """"-> bool": a function with a trailing return type, or a lambda."""
        position = index - 1
        while position >= 0:
            text = self.tokens[position].text
            if text in TRAILING_QUALIFIERS:
                position -= 1
                continue
            if text != ')':
                return
            opening = self.match_backward(position, '(', ')')
            if self.text(opening - 1) != 'noexcept':
                break
            position = opening - 2
        if position < 0:
            return
        opening = self.match_backward(position, '(', ')')
        if self.text(opening - 1) == ']':
            introducer = self.match_backward(opening - 1, '[', ']')
            holder = self.text(introducer - 2) if self.text(introducer - 1) == '=' and self.is_word(introducer - 2) else None
            self.record(index + 1, f'<lambda {holder}>' if holder else '<lambda>')
            return
        name = self.qualified_before(opening)
        if name is not None and name not in NOT_FUNCTION_NAMES:
            self.record(index + 1, name)


def bool_returns(path: str, text: str) -> list[Found]:
    """Every bool-returning declaration in one source, in line order."""
    return Scanner(path, tokens_of(code_only(text))).run()


def is_gated(path: str) -> bool:
    name = path.rpartition('/')[2]
    return any(path.startswith(directory + '/') for directory in GATED_PATHS) and not TEST_SOURCE.match(name)


def scan(root: Path, rev: str | None) -> list[Found]:
    paths = [path for path in check_style.owned_sources(root, rev) if is_gated(path)]
    found = []
    for path, data in check_style.read_sources(root, rev, paths).items():
        if b'\0' not in data:  # a binary file is no source
            found += bool_returns(path, data.decode('utf-8', errors='replace'))
    return found


@dataclass(frozen=True)
class Entry:
    file: str
    function: str
    count: int
    why: str | None

    @property
    def key(self) -> tuple[str, str]:
        return self.file, self.function


@dataclass(frozen=True)
class Baseline:
    lists: dict[str, dict[tuple[str, str], Entry]]  # each class's entries by (file, function)
    reclassified: dict[tuple[str, str], Entry] = field(default_factory=dict)  # by (file, function)

    @property
    def fallible(self) -> dict[tuple[str, str], Entry]:
        return self.lists[FALLIBLE]

    def kind_of(self, key: tuple[str, str]) -> str | None:
        return next((kind for kind in KINDS if key in self.lists[kind]), None)


def parse_entry(item: object, kind: str, name: str) -> Entry:
    if not isinstance(item, dict) or not set(item) <= ENTRY_FIELDS:
        raise ValueError(f'{name}: a "{kind}" entry has fields other than {", ".join(sorted(ENTRY_FIELDS))}: {item}')
    entry = Entry(item.get('file'), item.get('function'), item.get('count', 1), item.get('why'))
    where = f'{name}: {entry.file}: {entry.function}'
    if not isinstance(entry.file, str) or not isinstance(entry.function, str):
        raise ValueError(f'{name}: a "{kind}" entry names no file and function: {item}')
    if type(entry.count) is not int or entry.count < 1:
        raise ValueError(f'{where}: count is a whole number above 0')
    if entry.why is not None and not isinstance(entry.why, str):
        raise ValueError(f'{where}: "why" is a text')
    if kind != FALLIBLE and not (entry.why or '').strip():
        raise ValueError(f'{where}: an entry under "{kind}" says why ("why")')
    return entry


def parse_list(data: dict, kind: str, name: str) -> dict[tuple[str, str], Entry]:
    if not isinstance(data.get(kind, []), list):
        raise ValueError(f'{name}: "{kind}" is a list')
    entries = {}
    for item in data.get(kind, []):
        entry = parse_entry(item, kind, name)
        if entry.key in entries:
            raise ValueError(f'{name}: {entry.file}: {entry.function}: listed twice under "{kind}"')
        entries[entry.key] = entry
    return entries


def parse_baseline(text: str, name: str) -> Baseline:
    data = json.loads(text)
    if not isinstance(data, dict) or not set(KINDS) <= set(data) <= {*KINDS, RECLASSIFIED}:
        raise ValueError(f'{name}: the baseline holds exactly the lists {", ".join(KINDS)}, and "{RECLASSIFIED}" '
                         f'when it names an entry')
    lists = {kind: parse_list(data, kind, name) for kind in KINDS}
    for first, second in ((FALLIBLE, PREDICATES), (FALLIBLE, ABI), (PREDICATES, ABI)):
        both = sorted(set(lists[first]) & set(lists[second]))
        if both:
            raise ValueError(f'{name}: {both[0][0]}: {both[0][1]}: listed under "{first}" and "{second}"')
    reclassified = parse_list(data, RECLASSIFIED, name)
    unlisted = sorted(set(reclassified) - set(lists[PREDICATES]) - set(lists[ABI]))
    if unlisted:
        raise ValueError(f'{name}: {unlisted[0][0]}: {unlisted[0][1]}: listed under "{RECLASSIFIED}", but under neither '
                         f'"{PREDICATES}" nor "{ABI}"')
    return Baseline(lists, reclassified)


def read_baseline(root: Path, rev: str | None, path: Path | None) -> Baseline:
    """The file `path` names; otherwise, with `rev`, the baseline in that commit's tree, so each commit is checked
    against its own; otherwise the work tree's."""
    if path is None and rev:
        shown = check_style.git(root, 'show', f'{rev}:{BASELINE}')
        if shown.returncode:
            raise RuntimeError(f'{rev} has no {BASELINE}: {shown.stderr.strip()}')
        return parse_baseline(shown.stdout, f'{rev}:{BASELINE}')
    baseline = path or root / BASELINE
    return parse_baseline(baseline.read_text(encoding='utf-8'), baseline.name)


def previous_baseline(root: Path, since: str) -> tuple[Baseline | None, str]:
    """The baseline of the revision the fallible list is compared with, or None and why it cannot be read."""
    resolved = check_style.git(root, 'rev-parse', '--verify', '--quiet', f'{since}^{{commit}}')
    if resolved.returncode:
        return None, f'{since} is not a commit here (the first commit, or a shallow clone)'
    shown = check_style.git(root, 'show', f'{since}:{BASELINE}')
    if shown.returncode:
        return None, f'{since} has no {BASELINE}'
    return parse_baseline(shown.stdout, f'{since}:{BASELINE}'), ''


@dataclass
class Outcome:
    failures: list[str] = field(default_factory=list)
    stale: list[str] = field(default_factory=list)  # gone functions and fallen counts: --update drops or lowers them
    counts: Counter = field(default_factory=Counter)


def compare(found: list[Found], baseline: Baseline) -> Outcome:
    """The declarations found against the baseline: an unlisted function and a grown count fail; a gone function
    and a fallen count are stale."""
    outcome = Outcome()
    lines: dict[tuple[str, str], int] = {}
    for item in found:
        outcome.counts[item.key] += 1
        lines.setdefault(item.key, item.line)
    for key, count in sorted(outcome.counts.items()):
        where = f'{key[0]}:{lines[key]}: {key[1]}'
        kind = baseline.kind_of(key)
        if kind is None:
            outcome.failures.append(f'{where}: returns bool and is not in the baseline: return a Status or a Result; '
                                    f'if its false is an answer, never a failure, list it under "{PREDICATES}" with why')
            continue
        recorded = baseline.lists[kind][key].count
        if count > recorded:
            remedy = ('the fallible list stays empty' if kind == FALLIBLE else
                      f'raise its count only if the new one is under "{kind}" too')
            outcome.failures.append(f'{where}: {count} bool-returning declarations, its "{kind}" entry counts '
                                    f'{recorded}: {remedy}')
        elif count < recorded:
            outcome.stale.append(f'{key[0]}: {key[1]}: {count} bool-returning declarations, its "{kind}" entry '
                                 f'counts {recorded}')
    for kind in KINDS:
        for key in sorted(set(baseline.lists[kind]) - set(outcome.counts)):
            outcome.stale.append(f'{key[0]}: {key[1]}: listed under "{kind}", but returns no bool or is gone')
    return outcome


def fallible_listed(baseline: Baseline) -> list[str]:
    """Each fallible entry: #50 took the list to zero, and it stays empty."""
    return [f'{key[0]}: {key[1]}: listed under "{FALLIBLE}", which stays empty (#50 took it to zero): return a Status or '
            f'a Result (owned C code an enum wfg_status); if its false is an answer, never a failure, list it under '
            f'"{PREDICATES}" with why' for key in sorted(baseline.fallible)]


def grown(current: Baseline, previous: Baseline, since: str) -> list[str]:
    """Fallible entries of the previous revision's baseline moved to "predicates" or "abi", and predicate and abi
    counts raised, that "reclassified" does not name with as many declarations."""
    failures = []
    for kind in (PREDICATES, ABI):
        for key, entry in sorted(current.lists[kind].items()):
            was = previous.kind_of(key)
            if was == FALLIBLE:
                change = f'a fallible entry in {since}\'s baseline, now under "{kind}"'
            elif was is not None and entry.count > previous.lists[was][key].count:
                change = f'a "{kind}" count of {entry.count}, {since}\'s baseline has {previous.lists[was][key].count}'
            else:
                continue  # a new entry, or a move between the predicates and abi lists
            named = current.reclassified.get(key)
            if named is None or named.count < entry.count:
                failures.append(f'{key[0]}: {key[1]}: {change}: list it under "{RECLASSIFIED}" with why (and its count), '
                                f'so the change shows in review; or return a Status or a Result')
    return failures


def write_baseline(path: Path, baseline: Baseline, counts: Counter) -> None:
    """The baseline with gone entries dropped and counts lowered to those found; one entry per line."""
    def line(entry: Entry) -> str:
        item: dict[str, object] = {'file': entry.file, 'function': entry.function}
        count = min(entry.count, counts[entry.key])
        if count > 1:
            item['count'] = count
        if entry.why:
            item['why'] = entry.why
        return '\n    ' + json.dumps(item)

    lists = [f'  "{kind}": [' + ','.join(line(entry) for key, entry in sorted(baseline.lists[kind].items())
                                         if counts[key]) + '\n  ]' for kind in KINDS]
    # A reclassification stays while its predicate or abi entry does; the list is left out when empty.
    kept = [line(entry) for key, entry in sorted(baseline.reclassified.items()) if counts[key]]
    if kept:
        lists.append(f'  "{RECLASSIFIED}": [' + ','.join(kept) + '\n  ]')
    path.write_bytes(('{\n' + ',\n'.join(lists) + '\n}\n').encode('utf-8'))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--rev', help='Check this commit\'s tree and its baseline (for example HEAD) instead of the '
                                      'work tree')
    parser.add_argument('--since', help='Compare the predicate and abi lists with this revision\'s baseline (default: '
                                        'HEAD for the work tree, the first parent with --rev)')
    parser.add_argument('--baseline', type=Path,
                        help=f'The baseline to read (default: <root>/{BASELINE}, or with --rev {BASELINE} in that '
                             f'commit\'s tree)')
    parser.add_argument('--update', action='store_true',
                        help='Drop gone entries and lower fallen counts; only when nothing else fails')
    parser.add_argument('--list', action='store_true', help='Print every bool-returning declaration with its class')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.update and args.rev:
        print('--update rewrites the work tree\'s baseline and takes no --rev', file=sys.stderr)
        return 2
    since = args.since or (f'{args.rev}^1' if args.rev else 'HEAD')
    try:
        found = scan(root, args.rev)
        baseline = read_baseline(root, args.rev, args.baseline)
        previous, unread = previous_baseline(root, since)
    except (OSError, RuntimeError, ValueError) as error:
        print(f'Bool-return check failed to run: {error}', file=sys.stderr)
        return 2
    if args.list:
        for item in sorted(found, key=lambda item: (item.file, item.line)):
            print(f'{item.file}:{item.line}: {item.name}  [{baseline.kind_of(item.key) or "unlisted"}]')
    outcome = compare(found, baseline)
    failures = outcome.failures + fallible_listed(baseline) + (grown(baseline, previous, since) if previous else [])
    if not args.update:
        failures += [line + ': run --update to drop or lower it' for line in outcome.stale]
    for line in failures:
        print('FAIL ' + line)
    if args.update:
        for line in outcome.stale:
            print('drop ' + line)
    if previous is None:
        print(f'The predicate and abi lists are not compared with an earlier baseline: {unread}.')
    declared = {kind: sum(count for key, count in outcome.counts.items() if key in baseline.lists[kind])
                for kind in KINDS}
    where = f'tree of {args.rev}' if args.rev else 'work tree'
    print(f'Bool returns ({where}): {len(found)} bool-returning declarations; '
          f'{len(baseline.fallible)} fallible entries ({declared[FALLIBLE]} declarations; the list stays empty, #50), '
          f'{len(baseline.lists[PREDICATES])} predicates ({declared[PREDICATES]}), '
          f'{len(baseline.lists[ABI])} abi ({declared[ABI]}), {len(baseline.reclassified)} reclassified; '
          f'{len(failures)} failures.')
    if failures:
        return 1
    if args.update:
        target = args.baseline or root / BASELINE
        write_baseline(target, baseline, outcome.counts)
        print(f'Rewrote {target.name}.')
    elif outcome.stale:
        print(f'Run with --update to bring {BASELINE.name} to the tree.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
