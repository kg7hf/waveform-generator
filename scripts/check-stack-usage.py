#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Check, from GCC's call-graph files, that a task's deepest chain of calls fits its stack.

The player images compile every C and C++ source with -fstack-usage and
-fcallgraph-info=su (rt1170/CMakeLists.txt), so beside each object GCC writes
a .ci file: the object's functions, each with its frame in bytes and whether
that frame is fixed ("static"), and the calls each one makes; an indirect call
(a virtual function, a function pointer) is a call to __indirect_call. After
every player link this script reads the .ci files of the linked objects and
fails when:

- a function of the owned code (the repository's own sources, not
  third_party/ and not the vendor copies in the build) has a frame that is not
  static: a variable-length array or alloca;
- owned code recurses: a cycle of calls through owned functions, whose depth
  no frame bounds. (The JSON reader recursed once per nesting level until
  52bd33c. Vendor code keeps its own cycles, such as the NXP clock driver's
  CLOCK_GetFreq, unless the task reaches one.)
- from the task's entry function, a chain of calls reaches a cycle, an
  indirect call that no default or image-specific rule gives targets,
  or a local function of the image (its title "file:name") that no .ci file
  describes;
- a symbol the link wraps (--wrap, as GNU ld's --wrap=SYMBOL) has no
  __wrap_SYMBOL that a .ci file defines;
- the deepest chain from the entry, plus the reserve, is larger than the
  task's stack: the size of its stack array's input section in the link map.

A chain's depth is the sum of its functions' frames. A call to a complete-
object constructor or destructor (C1, D1) that GCC emits as an alias takes
the base-object one's (C2, D2), which the .ci file describes. A call to a
function no .ci file defines (newlib, libgcc) adds LIBRARY_FRAME_BYTES, and an indirect
call the deepest chain of the functions its rule names. A function that
several objects define (an inline function's out-of-line copies) takes the
largest of their frames and all of their calls. A call follows the link: for
each symbol the link wraps (--wrap SYMBOL, rt1170/CMakeLists.txt's
wfg_wrapped_symbols), a call to SYMBOL from an object that does not define
it reaches __wrap_SYMBOL (titled "*__wrap_SYMBOL" when an asm label names
it), as GNU ld sends only undefined references there, and a call to
__real_SYMBOL reaches SYMBOL. The reserve is what a task's
stack holds beside its calls on this core and port (Cortex-M7 with the FPU,
FreeRTOS's GCC ARM_CM7 r0p1 port; interrupts run on the main stack, not on a
task's): see the *_BYTES constants below.

A rule of the rules file names, by regular expressions on the functions'
names as GCC prints them (return type and parameters included), the callers
whose indirect calls it covers and the functions those calls may reach, with
why; "targets": null says the call is never made in these images.

Exit status 0: the check passes; 1: it fails, one line per failure; 2: it
could not run (a file missing or unreadable).
"""

from __future__ import annotations

import argparse
from collections.abc import Iterable
from dataclasses import dataclass, field
import importlib.util
import json
from pathlib import Path, PurePath
import re
import sys

# What a task's stack holds beside its calls.
# tasks.c aligns the top of the stack down to 8 B, and port.c's
# pxPortInitialiseStack starts one word below that: a task starts at most 8 B
# below the end of its stack array.
TOP_OFFSET_BYTES = 8
# An exception taken while the task runs stacks the core's extended frame on
# the task's stack (26 words: r0-r3, r12, lr, pc, xPSR, s0-s15, FPSCR and a
# reserved word), with 4 B more when the stack pointer is not 8-aligned.
EXCEPTION_FRAME_BYTES = 108
# A context switch then saves r4-r11 and r14 there, and s16-s31 for a task
# that used the FPU (port.c, xPortPendSVHandler): 25 words.
CONTEXT_SAVE_BYTES = 100
# configCHECK_FOR_STACK_OVERFLOW 2 (rt1170/os/FreeRTOSConfig.h): the stack's
# lowest 16 B must keep their fill pattern (stack_macros.h).
OVERFLOW_PATTERN_BYTES = 16
RESERVE_BYTES = TOP_OFFSET_BYTES + EXCEPTION_FRAME_BYTES + CONTEXT_SAVE_BYTES + OVERFLOW_PATTERN_BYTES
# A call into code GCC compiled elsewhere (newlib-nano, libgcc). The deepest
# such chain in the player image is __aeabi_uldivmod (16 B) calling
# __udivmoddi4 (32 B); memcpy and sqrt take 24 B, the others less.
LIBRARY_FRAME_BYTES = 64

INDIRECT = '__indirect_call'
# GNU ld's names for a wrapped symbol's wrapper and for the symbol itself, as a wrapper calls it.
WRAP_PREFIX = '__wrap_'
REAL_PREFIX = '__real_'
# GCC titles a function that an asm label names with the label behind a "*".
ASM_LABEL = '*'
# The mangled complete-object constructor and destructor, and the base-object ones they alias.
CONSTRUCTOR_ALIASES = (('C1E', 'C2E'), ('D1E', 'D2E'))
RULES = Path(__file__).resolve().with_name('stack-usage-rules.json')
ASSEMBLY_OBJECT = re.compile(r'\.(?:s|S|asm)\.(?:obj|o)$')

QUOTED = r'"((?:[^"\\]|\\.)*)"'  # a quoted text, with backslash escapes
NODE = re.compile(rf'^node: \{{ title: {QUOTED} label: {QUOTED}')
EDGE = re.compile(rf'^edge: \{{ sourcename: {QUOTED} targetname: {QUOTED}(?: label: {QUOTED})?')
FRAME = re.compile(r'^(\d+) bytes \(([\w,]+)\)$')


def _load(name: str, file_name: str):
    """A sibling script as a module."""
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().with_name(file_name))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


@dataclass
class Function:
    title: str
    name: str  # as GCC prints it: return type, qualified name, parameters
    source: str  # file:line:column of its definition
    frame: int
    qualifier: str
    calls: dict[str, str] = field(default_factory=dict)  # callee title -> the call's file:line:column


@dataclass
class CallGraph:
    functions: dict[str, Function] = field(default_factory=dict)  # by title, defined functions only
    wraps: tuple[str, ...] = ()  # the symbols the link wraps

    def add_node(self, title: str, label: str) -> bool:
        """Adds a function the node defines; whether it defines one (else it declares one its object calls)."""
        # A label is the name, the file:line:column and "N bytes (qualifier)", each ended by a backslash and an
        # n: split at the first and the last, as a Windows path can hold that pair too.
        name, _, rest = label.partition('\\n')
        source, _, last = rest.rpartition('\\n')
        frame = FRAME.match(last) if source else None
        if frame is None:
            return False  # a declaration: a function this object calls but does not define
        size, qualifier = int(frame.group(1)), frame.group(2)
        known = self.functions.get(title)
        if known is None:
            self.functions[title] = Function(title, name, source, size, qualifier)
        elif size > known.frame or qualifier != 'static':
            known.frame = max(known.frame, size)
            known.qualifier = qualifier if qualifier != 'static' else known.qualifier
        return True

    def add_edge(self, source: str, target: str, site: str) -> None:
        caller = self.functions.get(source)
        if caller is not None:
            caller.calls.setdefault(target, site)

    def wrapper(self, symbol: str) -> str | None:
        """The title of the wrapper of a wrapped symbol (__wrap_SYMBOL), or None when no .ci file defines it."""
        titles = (ASM_LABEL + WRAP_PREFIX + symbol, WRAP_PREFIX + symbol)
        return next((title for title in titles if title in self.functions), None)

    def linked(self, target: str, defined: set[str]) -> str:
        """The function a call to `target`, from an object that defines `defined`, reaches in the linked image."""
        name = target.removeprefix(ASM_LABEL)
        if name.startswith(REAL_PREFIX) and name.removeprefix(REAL_PREFIX) in self.wraps:
            return name.removeprefix(REAL_PREFIX)
        if target in self.wraps and target not in defined:
            return self.wrapper(target) or WRAP_PREFIX + target
        return target


def read_call_graph(texts: list[str], wraps: Iterable[str] = ()) -> CallGraph:
    """Every .ci file's functions and calls, merged, each call to the function the link gives it (`wraps`: the
    symbols the link wraps)."""
    graph = CallGraph(wraps=tuple(wraps))
    edges: list[tuple[str, str, str, set[str]]] = []
    for text in texts:
        defined: set[str] = set()  # the functions this object defines
        for line in text.splitlines():
            node = NODE.match(line)
            if node:
                if graph.add_node(node.group(1), node.group(2)):
                    defined.add(node.group(1))
                continue
            edge = EDGE.match(line)
            if edge:
                edges.append((edge.group(1), edge.group(2), edge.group(3) or '', defined))
    for source, target, site, defined in edges:
        graph.add_edge(source, graph.linked(target, defined), site)
    return graph


def call_graph_files(objects: list[Path]) -> list[Path]:
    """The .ci file of each C or C++ object (GCC names it after the object, without its extension)."""
    files = []
    for path in objects:
        if ASSEMBLY_OBJECT.search(path.name):
            continue
        ci = path.with_suffix('.ci')
        if not ci.is_file():
            raise FileNotFoundError(f'{path.name} has no {ci.name}: compile it with -fcallgraph-info=su')
        files.append(ci)
    return files


@dataclass(frozen=True)
class Rule:
    caller: re.Pattern
    targets: re.Pattern | None
    why: str
    caller_title: re.Pattern | None = None


def parse_rules(text: str, name: str) -> list[Rule]:
    data = json.loads(text)
    rules = []
    for item in data.get('indirect_calls', []) if isinstance(data, dict) else []:
        named = isinstance(item, dict) and isinstance(item.get('caller'), str)
        if not named or not str(item.get('why', '')).strip():
            raise ValueError(f'{name}: an indirect-call rule names its caller and says why: {item}')
        caller_title = item.get('caller_title')
        if 'caller_title' in item and not isinstance(caller_title, str):
            raise ValueError(f'{name}: "caller_title" is a regular-expression string when present: {item}')
        targets = item.get('targets')
        if targets is not None and not isinstance(targets, str):
            raise ValueError(f'{name}: "targets" is a pattern or null: {item}')
        pattern = re.compile(targets) if targets is not None else None
        title_pattern = re.compile(caller_title) if caller_title is not None else None
        rules.append(Rule(re.compile(item['caller']), pattern, item['why'], title_pattern))
    return rules


def is_owned(source: str, root: PurePath) -> bool:
    """Whether a function's file:line:column lies in the repository's own sources."""
    path = source.rsplit(':', 2)[0].replace('\\', '/').lower()
    base = str(root).replace('\\', '/').lower().rstrip('/') + '/'
    if not path.startswith(base):
        return False
    rest = path[len(base):]
    return not (rest.startswith('third_party/') or rest.startswith('build/'))


def entry_function(graph: CallGraph, name: str) -> Function | None:
    """The function a mangled name means: a global function's title, or the one local function's title (its
    file, ':', its name) that ends in it."""
    if name in graph.functions:
        return graph.functions[name]
    local = [function for title, function in graph.functions.items() if title.endswith(':' + name)]
    return local[0] if len(local) == 1 else None


def owned_cycles(graph: CallGraph, root: PurePath) -> list[list[str]]:
    """Each cycle of calls through owned functions, once (Tarjan's strongly connected components)."""
    owned = {title for title, function in graph.functions.items() if is_owned(function.source, root)}
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    cycles: list[list[str]] = []

    def callees(title: str) -> list[str]:
        return [target for target in graph.functions[title].calls if target in owned]

    for start in sorted(owned):
        if start in index:
            continue
        work = [(start, iter(callees(start)))]
        index[start] = low[start] = len(index)
        stack.append(start)
        on_stack.add(start)
        while work:
            title, pending = work[-1]
            advanced = False
            for target in pending:
                if target not in index:
                    index[target] = low[target] = len(index)
                    stack.append(target)
                    on_stack.add(target)
                    work.append((target, iter(callees(target))))
                    advanced = True
                    break
                if target in on_stack:
                    low[title] = min(low[title], index[target])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[title])
            if low[title] == index[title]:
                component = []
                while True:
                    member = stack.pop()
                    on_stack.discard(member)
                    component.append(member)
                    if member == title:
                        break
                if len(component) > 1 or title in callees(title):
                    cycles.append(sorted(component))
    return cycles


@dataclass
class Chain:
    depth: int
    functions: list[str]  # titles, the entry first


class ChainError(Exception):
    pass


def callee(graph: CallGraph, target: str) -> Function | None:
    """The function a call names; None for code GCC compiled elsewhere (newlib, libgcc). A complete-object
    constructor or destructor (C1, D1) that GCC emits as an alias of the base-object one (C2, D2) is the latter,
    which the .ci file describes. A local function of this image (its title "file:name") that no .ci file
    describes raises ChainError rather than count as a library call."""
    function = graph.functions.get(target)
    for alias, base in CONSTRUCTOR_ALIASES:
        if function is None and alias in target:
            function = graph.functions.get(target.replace(alias, base, 1))
    if function is None and ':' in target:
        raise ChainError(f'{target}: called, but no .ci file describes it')
    return function


def deepest_chain(graph: CallGraph, entry: Function, rules: list[Rule]) -> Chain:
    """The deepest chain of calls from `entry`; raises ChainError at a cycle or an indirect call no rule covers."""
    best: dict[str, Chain] = {}
    path: list[str] = []

    def targets_of(caller: Function, site: str) -> list[Function]:
        rule = next((rule for rule in rules if rule.caller.search(caller.name) and
                     (rule.caller_title is None or rule.caller_title.search(caller.title))), None)
        if rule is None:
            raise ChainError(f'{caller.name} ({site}) makes an indirect call that no rule in {RULES.name} covers')
        if rule.targets is None:
            return []
        found = [function for function in graph.functions.values() if rule.targets.search(function.name)]
        if not found:
            raise ChainError(f'{caller.name} ({site}): the targets of its rule ("{rule.targets.pattern}") name no '
                             f'function in this image')
        return found

    def deepest(function: Function) -> Chain:
        if function.title in best:
            return best[function.title]
        if function.title in path:
            cycle = path[path.index(function.title):] + [function.title]
            raise ChainError('a cycle of calls: ' + ' -> '.join(graph.functions[title].name for title in cycle))
        path.append(function.title)
        below = Chain(0, [])
        for target, site in function.calls.items():
            # A call names its callee as the callee's own node does: a local function by its file and name.
            callees = targets_of(function, site) if target == INDIRECT else [callee(graph, target)]
            for reached in callees:
                chain = deepest(reached) if reached is not None else Chain(LIBRARY_FRAME_BYTES, [target])
                if chain.depth > below.depth:
                    below = chain
        path.pop()
        chain = Chain(function.frame + below.depth, [function.title] + below.functions)
        best[function.title] = chain
        return chain

    return deepest(entry)


def stack_bytes(map_text: str, section: str) -> int:
    """The size of a task's stack: its stack array's input section in the link map."""
    placement = _load('check_memory_placement', 'check-memory-placement.py')
    placed = [item for item in placement.parse_map(map_text).sections if item.name == section]
    if len(placed) != 1:
        raise ValueError(f'{section}: placed {len(placed)} times in the map, expected once')
    return placed[0].size


@dataclass
class Outcome:
    failures: list[str] = field(default_factory=list)
    report: list[str] = field(default_factory=list)


def check(graph: CallGraph, root: PurePath, entry_name: str, stack: int, rules: list[Rule]) -> Outcome:
    outcome = Outcome()
    for function in sorted(graph.functions.values(), key=lambda item: item.source):
        if function.qualifier != 'static' and is_owned(function.source, root):
            outcome.failures.append(f'{function.source}: {function.name}: a {function.qualifier} frame '
                                    f'({function.frame} B): no variable-length array or alloca')
    for cycle in owned_cycles(graph, root):
        names = '; '.join(graph.functions[title].name for title in cycle)
        outcome.failures.append(f'owned code recurses, through {names}')
    for symbol in graph.wraps:
        if graph.wrapper(symbol) is None:
            outcome.failures.append(f'--wrap={symbol}: no .ci file defines {WRAP_PREFIX}{symbol}, which the link calls '
                                    f'in its place')
    entry = entry_function(graph, entry_name)
    if entry is None:
        outcome.failures.append(f'{entry_name}: no .ci file defines the entry function (or more than one does)')
        return outcome
    try:
        chain = deepest_chain(graph, entry, rules)
    except ChainError as error:
        outcome.failures.append(f'{entry.name}: {error}')
        return outcome
    needed = chain.depth + RESERVE_BYTES
    summary = (f'{entry.name}: deepest chain {chain.depth} B + reserve {RESERVE_BYTES} B = {needed} B '
               f'of a {stack} B stack')
    if needed > stack:
        outcome.failures.append(f'{summary}: {needed - stack} B too many')
    else:
        outcome.report.append(f'{summary}, {stack - needed} B spare')
    for title in chain.functions:
        function = graph.functions.get(title)
        outcome.report.append(f'  {function.frame if function else LIBRARY_FRAME_BYTES:6} '
                              f'{function.name if function else title + " (library)"}')
    return outcome


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--map', type=Path, required=True, help='GNU ld map (-Wl,-Map=...)')
    parser.add_argument('--entry', required=True, help='the task\'s entry function (its mangled name)')
    parser.add_argument('--stack-section', required=True, help='the input section of the task\'s stack array')
    parser.add_argument('--wrap', nargs='+', default=[], metavar='SYMBOL',
                        help='the symbols the link wraps (GNU ld\'s --wrap=SYMBOL)')
    parser.add_argument('--objects', type=Path, nargs='+', required=True, help='the linked objects')
    parser.add_argument('--rules', type=Path, default=RULES, help=f'the indirect-call rules (default {RULES.name})')
    parser.add_argument('--extra-rules', type=Path, action='append', default=[], metavar='FILE',
                        help='additional indirect-call rules for this image; may be repeated')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1],
                        help='the repository, whose sources outside third_party/ and build/ are owned')
    args = parser.parse_args()
    try:
        texts = [path.read_text(encoding='utf-8', errors='replace') for path in call_graph_files(args.objects)]
        graph = read_call_graph(texts, args.wrap)
        rules = parse_rules(args.rules.read_text(encoding='utf-8'), args.rules.name)
        for extra_rules in args.extra_rules:
            rules.extend(parse_rules(extra_rules.read_text(encoding='utf-8'), str(extra_rules)))
        stack = stack_bytes(args.map.read_text(encoding='utf-8', errors='replace'), args.stack_section)
    except (OSError, ValueError) as error:
        print(f'Stack usage check could not run: {error}', file=sys.stderr)
        return 2
    outcome = check(graph, PurePath(args.root), args.entry, stack, rules)
    for line in outcome.report:
        print(f'Stack usage: {line}')
    for line in outcome.failures:
        print(f'FAIL {line}', file=sys.stderr)
    return 1 if outcome.failures else 0


if __name__ == '__main__':
    sys.exit(main())
