#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check that owned C and C++ code fails one way: a Status or Result, never an error out-parameter (#50).

A fallible function returns m110::Status or m110::Result<T> (common/status.hpp),
whose message is a static text; the caller reads the reason from the value it
got. The style this replaced returned bool and wrote the reason through a
parameter, or kept it in an object for an accessor. The gate fails on either
in any of its forms:

- a pointer to a pointer to const char (const char** or char const**), the
  form signal-lab used for its static messages: owned code has no other use
  for the type;
- a reference to a pointer to const char (const char*&) whose name says it
  holds an error (the error names below, and what), the same out-parameter
  as a reference; a cursor such as parse_string(const char*& text, ...)
  passes;
- a string written through a non-const reference or a pointer whose name says
  it holds an error. The string is a std::string, std::string_view or another
  of the standard's string types (std::wstring, std::pmr::string), in any
  namespace or none, or a std::optional of one. The error names are error,
  err, message, msg, reason, why, failure or diagnostic, with a prefix or
  suffix of their own in snake case (replay_error, error_text) or camel case
  (errorText, lastError, errMsg), and also, as whole names, what, detail,
  details, text, e and out, which the old form used as well (an output text
  that is no error goes by another name, such as line or record). A const
  reference is an input and passes;
- a char buffer an error name (the first list) says the reason is written
  into: a char* (with or without a size beside it) or a std::span<char>
  (std::span<const char> is an input);
- a Status written through a non-const reference or a pointer parameter,
  whatever its name: bool f(..., m110::Status& status) is the old form with
  a Status for the text. A const reference is an input, and a function that
  returns a reference to a stored Status is no parameter;
- either half of a set_error()/error() pair, which keeps a failure's text in
  an object beside a bool result: a setter named set_ and an error name (in
  camel case set and one) whose parameter is a text (a char pointer or a
  string), and an accessor with no parameters, named with an error name or
  what, that returns a text (a char pointer or a string, by value or by
  reference, or a std::optional of a string). An object that latches a
  failure keeps a Status and gives it out as one (Engine::status()); a
  setter or an accessor of another type, set_critical_error_handler(handler)
  for one, passes;
- a function that returns its failure as text: a function, a member or not,
  named with an error name or what, that takes parameters and returns a text
  (as the accessor above). Only a function given the failure it names
  passes: its one parameter a Status, a StatusCode, or an error or reason
  enum (a type named ...Error or ...Reason), by value or by const reference.
  Those are the tables that publish a failure's name (rt1170/error_map.hpp's
  error_name(PlayerError) and error_name(EngineError), host/capture's
  reason_name(CompletionReason)) and a tool's text for a failed Status
  (signal-lab/validation/watterson_render.cpp's scenario_failure() and
  process_failure()). A parameter is declared with a type, so a variable
  initialised in parentheses (std::string message(text)) is no function.
  Nor is any in a block: a function's or a lambda's body, or an
  initialiser's or an enumeration's braces (check-bool-returns.py's scopes
  tell them from a namespace, a class and extern "C"). Owned code declares
  no function there, so std::string message(count * width, ' ') is a
  variable, although count * width reads as a declared pointer parameter.
  A trailing return type is not read: owned code writes the return type
  first;
- a C status read as a truth value. An owned C function that can fail
  returns an enum wfg_status (rt1170/platform/wfg_status.h), whose success,
  kWFG_StatusOk, is 0: a call left in the old bool form (if (!f()),
  if (f()), bool ok = f()) still compiles, satisfies warn_unused_result,
  and reads every success as a failure and every failure as a success. The
  functions are those any owned source declares or defines with the return
  type enum wfg_status (wfg_status in C++); a call of one fails as the
  operand of !, && or || or the condition of ?:, as the whole condition of
  an if or a while, and as what initialises a bool or is cast to one,
  unless the call is first compared or otherwise operated on
  (f() != kWFG_StatusOk && ready). A call passed on as it is (return f(),
  status_of(f()), enum wfg_status s = f()) passes. The text sees only a
  call; scripts/check-c-status-truth.py reads the AST and finds every form
  (a cast, a macro's expansion, a stored status). Its fixture,
  tests/c_status_truth, reads statuses as truth values on purpose, so this
  rule skips it (the others read it as any source);
- a C status function without __attribute__((warn_unused_result)), C's
  [[nodiscard]], which GCC cannot put on the type: then a caller may drop
  the failure without a word. Each declaration of a function that returns
  an enum wfg_status carries it (before or after the declarator, or
  [[nodiscard]]); a definition without it passes when a declaration of its
  name, in any owned source, carries it.

Comments and string and character literals are not code (check-style.py's
lexer blanks them first), so a comment that names the old form does not fail,
and a match over a blanked literal, such as the text a variable is
initialised with (std::string message("text")), is no declaration.
The owned sources are check-style.py's: the tracked C and C++ files of the
owned top-level directories, tests included. --rev reads a commit's tree in
place of the work tree. Exit status 0: none found; 1: findings, one per line
as path:line: code <- rule; 2: the check could not run.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
from collections.abc import Iterable
import importlib.util
from pathlib import Path
import re
import sys
from typing import NamedTuple

_STYLE = Path(__file__).resolve().with_name('check-style.py')
_spec = importlib.util.spec_from_file_location('check_style', _STYLE)
check_style = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_style)


def _load(name: str, file_name: str):
    """A sibling script as a module, under a name of its own (its dataclasses look their module up)."""
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().with_name(file_name))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# Its token pattern and its classify(), which tells what a "{" opens.
check_bool_returns = _load('wfg_error_style_bool_returns', 'check-bool-returns.py')

# The names a parameter that carries an error's text goes by: one of these
# words, in snake case with a prefix or suffix of its own (replay_error,
# error_text) or in camel case (errorText, lastError, errMsg).
ERROR_WORDS = 'error|err|message|msg|reason|why|failure|diagnostic'
ERROR_WORDS_CAPITALISED = 'Error|Err|Message|Msg|Reason|Why|Failure|Diagnostic'
ERROR_NAME = (rf'(?:(?:\w+_)?(?:{ERROR_WORDS})|[a-z][a-z0-9]*(?:[A-Z][a-z0-9]*)*?(?:{ERROR_WORDS_CAPITALISED}))'
              r's?(?:_\w+|[A-Z0-9]\w*)?')
# The other names an error text went by in the old form, as whole names
# only: a string written out (std::string& what, text, out ...), and a char
# pointer written out or an accessor read back (const char*& what, what()),
# where a cursor (const char*& text) and a text accessor (text()) pass.
TEXT_OUT_NAME = rf'(?:{ERROR_NAME}|what|details?|text|e|out)'
WHAT_NAME = rf'(?:{ERROR_NAME}|what)'
# A string type, in any namespace or none: std::string, std::string_view,
# std::wstring, std::pmr::string, string; or a std::optional of one.
STRING_TYPE = r'(?:\w+\s*::\s*)*(?:w|u8|u16|u32)?string(?:_view)?'
STRING_OR_OPTIONAL = rf'(?:{STRING_TYPE}|(?:\w+\s*::\s*)*optional\s*<\s*{STRING_TYPE}\s*>)'
# A text a function takes or gives: a pointer to char (const or not) or a string, by value or by reference.
TEXT_TYPE = rf'(?:(?:const\s+)?char(?:\s+const)?\s*\*|(?:const\s+)?{STRING_TYPE}(?:\s+const)?\s*&?)'
# A text a function returns: one of those, or a std::optional of a string (none for success).
RETURNED_TEXT = rf'(?:{TEXT_TYPE}|(?:\w+\s*::\s*)*optional\s*<\s*{STRING_TYPE}\s*>)'
# A parameter as a declaration writes it, where a call passes values: const (or volatile) first, or a type,
# perhaps a template, then a pointer or reference with or without a name, or a name.
TYPE_NAME = r'(?:\w+\s*::\s*)*\w+(?:\s*<[^<>()]*(?:<[^<>()]*>[^<>()]*)*>)?'
DECLARED_PARAMETER = rf'(?:const\b|volatile\b|{TYPE_NAME}(?:\s+const\b)?(?:\s*[*&]+\s*\w*|\s+\w+)\s*[,)=\[])'
# The one parameter of a function that names a failure it is given: a Status, its code, or an error or
# reason enum, by value or by const reference, named or not.
FAILURE_PARAMETER = (r'(?:const\s+)?(?:\w+\s*::\s*)*(?:Status|StatusCode|\w*Error|\w*Reason)\b(?:\s+const\b)?'
                     r'\s*&?\s*\w*\s*\)')
SETTER_NAME = (rf'(?:set_(?:\w+_)?(?:{ERROR_WORDS})s?(?:_\w+)?'
               rf'|set(?:[A-Z][a-z0-9]*)*?(?:{ERROR_WORDS_CAPITALISED})s?(?:[A-Z0-9]\w*)?)')
CONST_CHAR = r'\b(?:const\s+char|char\s+const)\s*\*\s*'
POINTER_TO_POINTER = 'a const char** (or char const**) parameter: return a Status with the static message'
POINTER_REFERENCE = 'a const char*& error parameter: return a Status with the static message'
STRING_OUT = 'an error text written through a reference or pointer: return a Status or a Result'
STATUS_OUT = 'a Status written through a reference or pointer parameter: return the Status, or a Result'
BUFFER_OUT = 'an error text written into a char buffer: return a Status with the static message'
ERROR_SETTER = 'an error text kept in an object (set_error()): return a Status; an object that latches a failure keeps a Status'
ERROR_ACCESSOR = 'an error text read back from an object (error()): return a Status from the call; a latched failure is given as a Status'
ERROR_RETURN = ('an error text a function returns: return a Status or a Result; only a function given the failure '
                '(a Status, its code, an error enum) may name it')
# Each rule: what it finds, its pattern, and whether a "const" just before the
# match (perhaps on the line before) makes the parameter an input.
RULES = (
    (POINTER_TO_POINTER, re.compile(CONST_CHAR + r'\*'), False),
    (POINTER_REFERENCE, re.compile(CONST_CHAR + r'&\s*' + WHAT_NAME + r'\b'), False),
    (STRING_OUT, re.compile(r'(?<!\w)' + STRING_OR_OPTIONAL + r'\s*[&*]\s*' + TEXT_OUT_NAME + r'\b'), True),
    # Any name: a parameter, so the name is followed by "," or ")", or by a
    # default of nullptr. A local reference ("Status& kept = ...") and a
    # function returning one ("Status& reason()") are no parameters.
    (STATUS_OUT, re.compile(r'(?<![\w:])(?:m110\s*::\s*)?Status\s*[&*]\s*\w+\s*(?:[,)]|=\s*nullptr\s*[,)])'), True),
    (BUFFER_OUT, re.compile(r'(?<![\w:])char\s*\*\s*' + ERROR_NAME + r'\b'), True),
    # A span of mutable characters is an out-buffer whatever its own const.
    (BUFFER_OUT, re.compile(r'(?<![\w:])(?:std\s*::\s*)?span\s*<\s*char\s*(?:,[^<>]*)?>\s*&?\s*' + ERROR_NAME + r'\b'), False),
    # A declaration or definition: the text parameter has a type and a name
    # (a call passes a value), qualified or not.
    (ERROR_SETTER, re.compile(r'\b' + SETTER_NAME + r'\s*\(\s*' + TEXT_TYPE + r'\s*\w+\s*[,)=]'), False),
    # An accessor with no parameters (a call has no type before it).
    (ERROR_ACCESSOR, re.compile(r'(?<!\w)' + RETURNED_TEXT + r'\s*(?:\w+\s*::\s*)*' + WHAT_NAME + r'\s*\(\s*(?:void\s*)?\)'),
     False),
    # A function with parameters, the first declared (a call or an initialiser passes values), unless its one
    # parameter is the failure it names.
    (ERROR_RETURN, re.compile(r'(?<!\w)' + RETURNED_TEXT + r'\s*(?:\w+\s*::\s*)*' + WHAT_NAME + r'\s*\(\s*(?!'
                              + FAILURE_PARAMETER + r')(?=' + DECLARED_PARAMETER + r')'), False),
)
# The rules that find a declaration only outside a block, where a name followed by parentheses can declare a
# function: in a block it initialises a variable.
DECLARATIONS_ONLY = frozenset((ERROR_RETURN,))
C_STATUS_TRUTH = ('a C status read as a truth value: kWFG_StatusOk is 0, so this reads a success as a failure; compare '
                  'with kWFG_StatusOk, or read it in C++ through status_of()')
C_STATUS_UNCHECKED = ('a function that returns an enum wfg_status without __attribute__((warn_unused_result)), C\'s '
                      '[[nodiscard]]: its callers may drop the failure; declare it so (a definition passes when a '
                      'declaration carries it)')
# scripts/check-c-status-truth.py's fixture: each truth read in it is there for that check to find (#51).
C_STATUS_TRUTH_FIXTURE = 'tests/c_status_truth/'
# What makes a dropped result a warning: GCC's attribute, or C23's and C++'s.
WARN_UNUSED = re.compile(r'\b(?:warn_unused_result|nodiscard)\b')
# A function that returns an enum wfg_status: its declaration or definition, C (enum wfg_status) or C++
# (wfg_status), gives its name.
C_STATUS_FUNCTION = re.compile(r'(?<![\w:])wfg_status\s+(\w+)\s*\(')
# What binds a call tighter than a truth value would: a comparison, an arithmetic or a bitwise operator, before or
# after it (&& and || excluded).
TIGHTER_AFTER = re.compile(r'(?:==|!=|<=|>=|<|>|[-+*/%^]|&(?!&)|\|(?!\|))')
TIGHTER_BEFORE = re.compile(r'(?:==|!=|<=|>=|<|>|[-+*/%^]|(?<!&)&|(?<!\|)\|)$')
# An if's or a while's "(", which the call fills.
CONDITION = re.compile(r'\b(?:if|while)\s*\($')
# A bool the call initialises (= f(), {f()}, (f())) or a cast to bool.
BOOL_FROM = re.compile(r'(?:\bbool\s+\w+\s*(?:=|\{|\()|\bbool\s*>\s*\()$')


class CStatusFunctions(NamedTuple):
    """The functions the owned sources declare or define to return an enum wfg_status, and those of them a
    declaration or a definition gives warn_unused_result."""
    names: frozenset[str] = frozenset()
    attributed: frozenset[str] = frozenset()


class CStatusDeclaration(NamedTuple):
    """A declaration or a definition of a C status function in one source."""
    name: str
    offset: int
    attributed: bool  # it carries warn_unused_result (or [[nodiscard]])
    definition: bool


def closing_paren(code: str, opening: int) -> int:
    """The offset of the ")" that closes the "(" at `opening`, or the code's length."""
    depth = 0
    for index in range(opening, len(code)):
        depth += (code[index] == '(') - (code[index] == ')')
        if depth == 0:
            return index
    return len(code)


def c_status_declarations(code: str) -> list[CStatusDeclaration]:
    """Each declaration or definition of a C status function in the code: the statement from the last ";", "{"
    or "}" before its return type up to its ";" or its body's "{" holds its attributes."""
    found = []
    for match in C_STATUS_FUNCTION.finditer(code):
        head = code[max(code.rfind(mark, 0, match.start()) for mark in ';{}') + 1:match.start()]
        rest = code[closing_paren(code, match.end() - 1) + 1:]
        end = min((index for index in (rest.find(';'), rest.find('{')) if index >= 0), default=len(rest))
        attributed = WARN_UNUSED.search(head) is not None or WARN_UNUSED.search(rest, 0, end) is not None
        found.append(CStatusDeclaration(match.group(1), match.start(), attributed, rest[end:end + 1] == '{'))
    return found


def c_status_functions(texts: Iterable[str]) -> CStatusFunctions:
    """Every function a source declares or defines with the return type enum wfg_status (wfg_status in C++)."""
    declarations = [declaration for text in texts for declaration in c_status_declarations(check_style.code_only(text))]
    return CStatusFunctions(frozenset(declaration.name for declaration in declarations),
                            frozenset(declaration.name for declaration in declarations if declaration.attributed))


def c_status_unchecked(code: str, c_statuses: CStatusFunctions) -> list[int]:
    """The offsets of the declarations of C status functions without warn_unused_result, and of the definitions
    without it whose name no declaration gives it."""
    return [declaration.offset for declaration in c_status_declarations(code) if not declaration.attributed and
            (not declaration.definition or declaration.name not in c_statuses.attributed)]


def read_as_truth(code: str, start: int, end: int) -> bool:
    """Whether the call at code[start:end] is read as a truth value."""
    before = code[:start].rstrip()
    after = code[end:].lstrip()
    if before.endswith('!'):  # a "!=" before it ends with "="
        return True
    if TIGHTER_BEFORE.search(before) or TIGHTER_AFTER.match(after):
        return False
    if before.endswith(('&&', '||')) or after.startswith(('&&', '||', '?')):
        return True
    if CONDITION.search(before) and after.startswith(')'):
        return True
    return BOOL_FROM.search(before) is not None


def c_status_truths(code: str, c_statuses: CStatusFunctions) -> list[int]:
    """The offsets of the calls of a C status function that read its status as a truth value. A declaration or a
    definition is no call to read: a type, never an operator, comes before its name."""
    if not c_statuses.names:
        return []
    names = '|'.join(sorted(map(re.escape, c_statuses.names)))
    call = re.compile(rf'(?<![\w.])(?:::\s*)?(?:{names})\s*\(')
    return [match.start() for match in call.finditer(code)
            if read_as_truth(code, match.start(), closing_paren(code, match.end() - 1) + 1)]


CONST_BEFORE = re.compile(r'\bconst\s*$')
QUOTES = frozenset('"\'')


def block_spans(code: str) -> list[tuple[int, int]]:
    """Where the code is in a block: each "{" that a namespace, a class, extern "C" or the file opens and
    check-bool-returns.py's classify() calls no namespace, class or linkage block (a function's or a lambda's
    body, an initialiser's or an enumeration's braces), to its "}", in order. The code has its comments, literals
    and preprocessor directives blanked (check-bool-returns.py's code_only()), so its offsets are the text's."""
    tokens = [(match.lastgroup, match.group(), match.start()) for match in check_bool_returns.TOKEN.finditer(code)]
    spans: list[tuple[int, int]] = []
    # Each open "{": its offset when it opens a block from a declarative scope, else None (a namespace, a class or
    # a linkage block, or a brace inside a block).
    opened: list[int | None] = []
    block_start: int | None = None  # the outermost open block's "{"
    statement_start = 0
    parens = 0
    for index, (_, word, offset) in enumerate(tokens):
        if word == '{':
            if block_start is not None:
                opened.append(None)
                continue
            head = [check_bool_returns.Token(kind, text, 0) for kind, text, _ in tokens[statement_start:index]]
            declarative = check_bool_returns.classify(head).kind in check_bool_returns.DECLARATIVE_SCOPES
            opened.append(None if declarative else offset)
            block_start = None if declarative else offset
            statement_start, parens = index + 1, 0
        elif word == '}':
            start = opened.pop() if opened else None
            if start is not None:
                spans.append((start, offset + 1))
                block_start = None
            if block_start is None:
                statement_start, parens = index + 1, 0
        elif block_start is not None:
            continue
        elif word == '(':
            parens += 1
        elif word == ')':
            parens = max(parens - 1, 0)
        elif word == ';' and parens == 0:
            statement_start = index + 1
    return spans


def in_block(spans: list[tuple[int, int]], offset: int) -> bool:
    """Whether `offset` lies in one of the spans (in order, none inside another)."""
    index = bisect_right(spans, offset, key=lambda span: span[0]) - 1
    return index >= 0 and spans[index][0] <= offset < spans[index][1]


def covers_literal(text: str, code: str, start: int, end: int) -> bool:
    """Whether code[start:end] covers a string or character literal the lexer blanked (a quote there in the
    text, a space in the code)."""
    return any(text[index] in QUOTES and code[index] == ' ' for index in range(start, end))


def findings(path: str, text: str, c_statuses: CStatusFunctions = CStatusFunctions()) -> list[str]:
    """The old form's occurrences in one source, each as path:line: code <- rule, in line order; c_statuses are
    the C status functions of all the owned sources."""
    code = check_style.code_only(text)
    spans = block_spans(check_bool_returns.code_only(text))
    lines = text.split('\n')
    found = []
    truths = [] if path.startswith(C_STATUS_TRUTH_FIXTURE) else c_status_truths(code, c_statuses)
    for description, offsets in ((C_STATUS_TRUTH, truths),
                                 (C_STATUS_UNCHECKED, c_status_unchecked(code, c_statuses))):
        for offset in offsets:
            number = code.count('\n', 0, offset) + 1
            found.append((number, f'{path}:{number}:{lines[number - 1].rstrip()}  <- {description}'))
    for description, rule, input_when_const in RULES:
        for match in rule.finditer(code):
            if input_when_const and CONST_BEFORE.search(code, 0, match.start()):
                continue
            if description in DECLARATIONS_ONLY and in_block(spans, match.start()):
                continue
            if covers_literal(text, code, match.start(), match.end()):
                continue
            number = code.count('\n', 0, match.start()) + 1
            found.append((number, f'{path}:{number}:{lines[number - 1].rstrip()}  <- {description}'))
    return [line for _, line in sorted(found)]


def check(root: Path, rev: str | None) -> list[str]:
    sources = {path: data.decode('utf-8', errors='replace')
               for path, data in check_style.read_sources(root, rev, check_style.owned_sources(root, rev)).items()
               if b'\0' not in data}  # a binary file is no source
    c_statuses = c_status_functions(sources.values())
    result = []
    for path, text in sources.items():
        result += findings(path, text, c_statuses)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--rev', help='Check this commit\'s tree (for example HEAD) instead of the work tree')
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        found = check(root, args.rev)
    except (OSError, RuntimeError, ValueError) as error:
        print(f'Error-style check failed to run: {error}', file=sys.stderr)
        return 2
    for finding in found:
        print(finding)
    where = f'tree of {args.rev}' if args.rev else 'work tree'
    truths = sum(1 for finding in found if finding.endswith(C_STATUS_TRUTH))
    unchecked = sum(1 for finding in found if finding.endswith(C_STATUS_UNCHECKED))
    print(f'Error style ({where}): {len(found) - truths - unchecked} error out-parameters or kept or returned error '
          f'texts, {truths} C statuses read as a truth value, {unchecked} C status functions without '
          f'warn_unused_result, in owned C/C++ sources.')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
