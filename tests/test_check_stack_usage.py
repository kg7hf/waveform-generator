#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""scripts/check-stack-usage.py against call-graph excerpts in GCC 14's .ci format.

Each excerpt follows arm-none-eabi-gcc 14.2's -fcallgraph-info=su output, as
the player link reads it (rt1170/CMakeLists.txt): a node per function, its
label the name, the file:line:column and "N bytes (static)"; a node with the
name alone for a function the object calls but does not define; an edge per
call, labelled with the call's file:line:column; a local function's title
prefixed with its file; an indirect call made to __indirect_call.

The repository's rules for the USB task are checked against their sources:
each TinyUSB caller's targets are exactly the callbacks of the fields it calls
in the vendored usbd.c's _usbd_driver entries that rt1170/usb/tusb_config.h
builds (CDC, MSC and audio), the DD008 session's are the callbacks
rt1170/tx_usb.cpp passes, the EP0 order glue's are the forward and the poll
rt1170/usb/usb_tinyusb_port.c passes, critical_error()'s is the handler
rt1170/critical_error.cpp installs, and the release images check the USB
task's stack beside the player task's, following the symbols the link wraps
(--wrap): a call to one reaches its wrapper unless its own object defines
it, and a call to __real_SYMBOL reaches SYMBOL.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'check-stack-usage.py'
_spec = importlib.util.spec_from_file_location('check_stack_usage', SCRIPT)
stack_usage = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = stack_usage  # dataclasses resolve their module by name
_spec.loader.exec_module(stack_usage)

ROOT = PurePosixPath('/repo')
OWNED = '/repo/rt1170/player.cpp'
VENDOR = '/repo/third_party/mcux-sdk/drivers/fsl_clock.c'
COPIED = '/repo/build/main/fw/vendor/sdmmc/sd/fsl_sd.c'
ENTRY = '_Z10task_entryPv'
RESERVE = stack_usage.RESERVE_BYTES
LIBRARY = stack_usage.LIBRARY_FRAME_BYTES


def node(title: str, frame: int | None = None, source: str = OWNED, name: str | None = None,
         qualifier: str = 'static') -> str:
    shown = name or title
    if frame is None:
        return f'node: {{ title: "{title}" label: "{shown}" }}'
    return f'node: {{ title: "{title}" label: "{shown}\\n{source}:1:1\\n{frame} bytes ({qualifier})" }}'


def edge(source: str, target: str, site: str = f'{OWNED}:2:3') -> str:
    return f'edge: {{ sourcename: "{source}" targetname: "{target}" label: "{site}" }}'


def graph(*lines: str) -> stack_usage.CallGraph:
    return stack_usage.read_call_graph(['graph: { title: "x.cpp"\n' + '\n'.join(lines) + '\n}\n'])


def rules(*items: dict) -> list:
    return stack_usage.parse_rules(json.dumps({'indirect_calls': list(items)}), 'rules.json')


def run(call_graph: stack_usage.CallGraph, stack: int, indirect: list | None = None) -> stack_usage.Outcome:
    return stack_usage.check(call_graph, ROOT, ENTRY, stack, indirect or [])


# The task: its entry (100 B) calls a (200 B) and b (50 B); a calls c (300 B), which calls memcpy, a
# library function no .ci defines. The deepest chain is entry, a, c and memcpy.
CHAIN = (node(ENTRY, 100), node('a', 200), node('b', 50), node('c', 300), node('memcpy'),
         edge(ENTRY, 'a'), edge(ENTRY, 'b'), edge('a', 'c'), edge('c', 'memcpy'))
DEEPEST = 100 + 200 + 300 + LIBRARY


class ChainTests(unittest.TestCase):
    def test_the_deepest_chain_and_the_reserve_fill_the_stack_exactly(self):
        outcome = run(graph(*CHAIN), DEEPEST + RESERVE)
        self.assertEqual(outcome.failures, [])
        self.assertIn(f'deepest chain {DEEPEST} B + reserve {RESERVE} B = {DEEPEST + RESERVE} B', outcome.report[0])
        self.assertIn('0 B spare', outcome.report[0])
        self.assertEqual([line.split()[0] for line in outcome.report[1:]], ['100', '200', '300', str(LIBRARY)])

    def test_one_byte_more_than_the_stack_fails(self):
        outcome = run(graph(*CHAIN), DEEPEST + RESERVE - 1)
        self.assertEqual(len(outcome.failures), 1)
        self.assertIn('1 B too many', outcome.failures[0])

    def test_the_reserve_is_the_exception_frame_the_context_save_and_the_ends(self):
        self.assertEqual(RESERVE, 8 + 108 + 100 + 16)

    def test_a_task_entry_that_is_a_local_function_is_found_by_its_name(self):
        local = f'{OWNED}:{ENTRY}'
        outcome = run(graph(node(local, 40), node('a', 8), edge(local, 'a')), 48 + RESERVE)
        self.assertEqual(outcome.failures, [])

    def test_a_missing_entry_fails(self):
        outcome = stack_usage.check(graph(*CHAIN), ROOT, '_Z5otherv', 1 << 20, [])
        self.assertIn('no .ci file defines the entry function', outcome.failures[0])

    def test_an_inline_function_defined_twice_takes_the_larger_frame(self):
        two_copies = stack_usage.read_call_graph([
            '\n'.join((node(ENTRY, 10), node('_Z6inlinev', 30), edge(ENTRY, '_Z6inlinev'))),
            '\n'.join((node('_Z6inlinev', 70), node('d', 5), edge('_Z6inlinev', 'd'))),
        ])
        outcome = run(two_copies, 10 + 70 + 5 + RESERVE)
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 85 B', outcome.report[0])

    def test_a_complete_object_constructor_is_its_base_object_alias(self):
        # GCC describes C2 (and D2) and makes C1 (D1) an alias of it: a call to C1 takes C2's frame.
        local = f'{OWNED}:_ZN4m1106ResultIhEC'
        constructed = graph(node(ENTRY, 100), node(local + '2ENS_6StatusE', 300), edge(ENTRY, local + '1ENS_6StatusE'))
        outcome = run(constructed, 1 << 20)
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 400 B', outcome.report[0])
        destroyed = graph(node(ENTRY, 100), node('_ZN1SD2Ev', 500), edge(ENTRY, '_ZN1SD1Ev'))
        self.assertIn('deepest chain 600 B', run(destroyed, 1 << 20).report[0])

    def test_a_local_function_no_call_graph_describes_fails(self):
        outcome = run(graph(node(ENTRY, 100), edge(ENTRY, f'{OWNED}:_ZN12_GLOBAL__N_14lostEv')), 1 << 20)
        self.assertEqual(len(outcome.failures), 1)
        self.assertIn('_ZN12_GLOBAL__N_14lostEv: called, but no .ci file describes it', outcome.failures[0])

    def test_a_path_with_a_backslash_n_keeps_its_name_and_frame(self):
        windows = r'D:\repo\new\x.cpp'
        call_graph = graph(node(ENTRY, 24, source=windows, name='void entry(void*)'))
        self.assertEqual(call_graph.functions[ENTRY].frame, 24)
        self.assertEqual(call_graph.functions[ENTRY].name, 'void entry(void*)')
        self.assertEqual(call_graph.functions[ENTRY].source, windows + ':1:1')


class WrapTests(unittest.TestCase):
    """GNU ld's --wrap=SYMBOL, as the player link wraps TinyUSB's dcd_event_handler (rt1170/CMakeLists.txt): a call
    to SYMBOL from an object that does not define it reaches __wrap_SYMBOL, which GCC titles "*__wrap_SYMBOL" when an
    asm label names it; a call to __real_SYMBOL reaches SYMBOL; a call in SYMBOL's own object is not wrapped."""

    # The MSC driver's object calls dcd_event_handler, which usbd.c's object defines (48 B, beside a function of its
    # own that calls it); the port's object defines the wrapper (0 B), which calls the glue's router (72 B), whose
    # indirect call is the forward to the real handler.
    MSC = '\n'.join((node(ENTRY, 100, VENDOR, name='void task_entry(void*)'), node('dcd_event_handler'),
                     edge(ENTRY, 'dcd_event_handler')))
    USBD = '\n'.join((node('dcd_event_handler', 48, VENDOR), node('usbd_internal', 8, VENDOR),
                      edge('usbd_internal', 'dcd_event_handler')))
    PORT = '\n'.join((node('*__wrap_dcd_event_handler', 0, name='wfg_wrap_dcd_event_handler'), node('route', 72),
                      edge('*__wrap_dcd_event_handler', 'route'), edge('route', '__indirect_call', site=f'{OWNED}:7:7')))
    FORWARD = {'caller': '^route$', 'targets': '^dcd_event_handler$', 'why': 'the forward: __real_dcd_event_handler'}
    WRAPS = ('dcd_event_handler',)

    def test_a_wrapped_call_goes_through_the_wrapper_to_the_real_handler(self):
        call_graph = stack_usage.read_call_graph([self.MSC, self.USBD, self.PORT], self.WRAPS)
        outcome = run(call_graph, 1 << 20, rules(self.FORWARD))
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 220 B', outcome.report[0])
        self.assertEqual([line.split()[-1] for line in outcome.report[1:]],
                         ['task_entry(void*)', 'wfg_wrap_dcd_event_handler', 'route', 'dcd_event_handler'])

    def test_a_wrapper_defined_by_its_own_name_is_found_too(self):
        plain = self.PORT.replace('"*__wrap_dcd_event_handler"', '"__wrap_dcd_event_handler"')
        call_graph = stack_usage.read_call_graph([self.MSC, self.USBD, plain], self.WRAPS)
        outcome = run(call_graph, 1 << 20, rules(self.FORWARD))
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 220 B', outcome.report[0])

    def test_without_the_wrap_the_call_reaches_the_real_handler_alone(self):
        call_graph = stack_usage.read_call_graph([self.MSC, self.USBD, self.PORT])
        self.assertIn('deepest chain 148 B', run(call_graph, 1 << 20, rules(self.FORWARD)).report[0])

    def test_a_call_in_the_wrapped_symbol_s_own_object_is_not_wrapped(self):
        task = '\n'.join((node(ENTRY, 100), node('usbd_internal'), edge(ENTRY, 'usbd_internal')))
        call_graph = stack_usage.read_call_graph([task, self.USBD, self.PORT], self.WRAPS)
        self.assertIn('deepest chain 156 B', run(call_graph, 1 << 20, rules(self.FORWARD)).report[0])

    def test_a_call_to_real_reaches_the_symbol(self):
        task = '\n'.join((node(ENTRY, 100), node('*__real_dcd_event_handler'), edge(ENTRY, '*__real_dcd_event_handler')))
        call_graph = stack_usage.read_call_graph([task, self.USBD, self.PORT], self.WRAPS)
        outcome = run(call_graph, 1 << 20)
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 148 B', outcome.report[0])

    def test_a_wrapped_symbol_whose_wrapper_no_call_graph_defines_fails(self):
        call_graph = stack_usage.read_call_graph([self.MSC, self.USBD], self.WRAPS)
        outcome = run(call_graph, 1 << 20)
        self.assertEqual(len(outcome.failures), 1, outcome.failures)
        self.assertIn('--wrap=dcd_event_handler: no .ci file defines __wrap_dcd_event_handler', outcome.failures[0])


class RecursionTests(unittest.TestCase):
    def test_owned_recursion_fails_although_the_task_does_not_reach_it(self):
        outcome = run(graph(*CHAIN, node('p', 8), node('q', 8), edge('p', 'q'), edge('q', 'p')), 1 << 20)
        self.assertEqual(len(outcome.failures), 1)
        self.assertIn('owned code recurses, through p; q', outcome.failures[0])

    def test_a_function_that_calls_itself_recurses(self):
        outcome = run(graph(*CHAIN, node('p', 8), edge('p', 'p')), 1 << 20)
        self.assertIn('owned code recurses, through p', outcome.failures[0])

    def test_a_vendor_cycle_the_task_does_not_reach_passes(self):
        cycle = (node('clock', 8, VENDOR), node('root', 8, VENDOR), edge('clock', 'root'), edge('root', 'clock'))
        self.assertEqual(run(graph(*CHAIN, *cycle), 1 << 20).failures, [])

    def test_a_vendor_cycle_the_task_reaches_fails(self):
        cycle = (node('clock', 8, COPIED), node('root', 8, COPIED), edge('clock', 'root'), edge('root', 'clock'))
        outcome = run(graph(*CHAIN, *cycle, edge('b', 'clock')), 1 << 20)
        self.assertEqual(len(outcome.failures), 1)
        self.assertIn('a cycle of calls: clock -> root -> clock', outcome.failures[0])

    def test_the_reader_recursion_of_52bd33c_parent_fails(self):
        # The JSON reader before 52bd33c: three frames per nesting level.
        reader = (node('parse_value', 96), node('read_value', 32), node('parse_object', 64), node('parse_array', 56),
                  edge(ENTRY, 'parse_value'), edge('parse_value', 'read_value'), edge('read_value', 'parse_object'),
                  edge('read_value', 'parse_array'), edge('parse_object', 'parse_value'),
                  edge('parse_array', 'parse_value'))
        outcome = run(graph(node(ENTRY, 100), *reader), 1 << 20)
        self.assertEqual(len(outcome.failures), 2)
        self.assertIn('through parse_array; parse_object; parse_value; read_value', outcome.failures[0])
        self.assertIn('a cycle of calls: parse_value -> read_value -> parse_object -> parse_value', outcome.failures[1])


class FrameTests(unittest.TestCase):
    def test_a_dynamic_owned_frame_fails(self):
        outcome = run(graph(*CHAIN, node('vla', 16, qualifier='dynamic,bounded')), 1 << 20)
        self.assertEqual(len(outcome.failures), 1)
        self.assertIn('vla: a dynamic,bounded frame (16 B)', outcome.failures[0])

    def test_a_dynamic_vendor_frame_passes(self):
        self.assertEqual(run(graph(*CHAIN, node('vla', 16, VENDOR, qualifier='dynamic')), 1 << 20).failures, [])


class IndirectCallTests(unittest.TestCase):
    TASK = (node(ENTRY, 100, name='void task_entry(void*)'), node('engine', 40, name='void Engine::run()'),
            node('stage1', 500, name='virtual void Stage1::process()'),
            node('stage2', 900, name='virtual void Stage2::process()'), node('other', 5000, name='void other()'),
            edge(ENTRY, 'engine'), edge('engine', '__indirect_call', site=f'{OWNED}:9:9'))
    ENGINE = {'caller': 'Engine::', 'targets': r'^virtual .*::process\(', 'why': 'the stages'}

    def test_an_indirect_call_no_rule_covers_fails(self):
        outcome = run(graph(*self.TASK), 1 << 20)
        self.assertEqual(len(outcome.failures), 1)
        self.assertIn(f'void Engine::run() ({OWNED}:9:9) makes an indirect call that no rule', outcome.failures[0])

    def test_an_indirect_call_takes_the_deepest_function_its_rule_names(self):
        outcome = run(graph(*self.TASK), 1 << 20, rules(self.ENGINE))
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 1040 B', outcome.report[0])

    def test_a_rule_without_targets_adds_nothing(self):
        outcome = run(graph(*self.TASK), 1 << 20, rules(dict(self.ENGINE, targets=None)))
        self.assertIn('deepest chain 140 B', outcome.report[0])

    def test_a_rule_whose_targets_name_no_function_fails(self):
        outcome = run(graph(*self.TASK), 1 << 20, rules(dict(self.ENGINE, targets='Gone::process')))
        self.assertIn('name no function in this image', outcome.failures[0])

    def test_a_rule_says_why(self):
        with self.assertRaises(ValueError):
            rules({'caller': 'Engine::', 'targets': None, 'why': ' '})

    def test_a_rule_can_constrain_the_gcc_graph_title_as_well_as_its_printed_name(self):
        extra_path = Path(__file__).resolve().parents[1] / 'examples' / 'encoder-plugin' / 'stack-usage-rules.json'
        extra = stack_usage.parse_rules(extra_path.read_text(encoding='utf-8'), str(extra_path))
        entry = node(ENTRY, 10, name='void task_entry(void*)')
        constructor_name = 'waveform_source::Encoder* waveform_generator::{anonymous}::construct(void*, std::size_t)'
        destructor_name = 'constexpr waveform_example::ToneEncoder::~ToneEncoder()'
        part_title = '/repo/rt1170/tx_artifact.cpp:_ZN18waveform_generator12_GLOBAL__N_116start_generationEv.part.0'
        matching = graph(entry,
                         node(part_title, 20, OWNED, name='0()'),
                         node('tone_constructor', 30, OWNED, name=constructor_name),
                         node('tone_destructor', 40, OWNED, name=destructor_name),
                         edge(ENTRY, part_title), edge(part_title, '__indirect_call'))
        outcome = stack_usage.check(matching, ROOT, ENTRY, 1 << 20, extra)
        self.assertEqual(outcome.failures, [])
        self.assertIn('waveform_example::ToneEncoder::~ToneEncoder()', '\n'.join(outcome.report))

        other_title = '/repo/rt1170/other.cpp:_ZN18waveform_generator12_GLOBAL__N_110other_callEv.part.0'
        unrelated = graph(entry,
                          node(other_title, 20, OWNED, name='0()'),
                          node('tone_constructor', 30, OWNED, name=constructor_name),
                          node('tone_destructor', 40, OWNED, name=destructor_name),
                          edge(ENTRY, other_title), edge(other_title, '__indirect_call'))
        failed = stack_usage.check(unrelated, ROOT, ENTRY, 1 << 20, extra)
        self.assertIn('makes an indirect call that no rule', failed.failures[0])

    def test_a_caller_title_must_be_a_regular_expression_string(self):
        for invalid in (None, 1):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                rules({'caller': 'caller', 'caller_title': invalid, 'targets': None, 'why': 'for the test'})

    def test_the_repository_rules_parse_and_each_says_why(self):
        parsed = stack_usage.parse_rules(stack_usage.RULES.read_text(encoding='utf-8'), stack_usage.RULES.name)
        self.assertGreater(len(parsed), 0)
        self.assertTrue(all(rule.why.strip() for rule in parsed))


class UsbTaskRuleTests(unittest.TestCase):
    """The repository's rules for the USB task's indirect calls: TinyUSB's calls through the usbd_class_driver_t of
    each class driver the player's rt1170/usb/tusb_config.h builds (read from the vendored usbd.c's _usbd_driver
    table), and the DD008 session's callbacks; and the release images check the USB task's stack beside the player
    task's (rt1170/CMakeLists.txt)."""

    REPOSITORY = Path(__file__).resolve().parents[1]
    USBD = REPOSITORY / 'third_party' / 'tinyusb' / 'src' / 'device' / 'usbd.c'
    TUSB_CONFIG = REPOSITORY / 'rt1170' / 'usb' / 'tusb_config.h'
    PLAYER_DRIVERS = {'CDC', 'MSC', 'AUDIO'}
    # Which usbd_class_driver_t fields each TinyUSB function calls through, as the player image's call graph names
    # the function that makes the call: process_control_request(), invoke_class_control(), process_set_config()
    # and usbd_control_xfer_cb() are inlined into tud_task_ext. No task calls tud_deinit (deinit).
    FIELDS_BY_CALLER = {
        'tud_rhport_init': ('init',),
        'configuration_reset': ('reset',),
        'tud_task_ext': ('open', 'control_xfer_cb', 'xfer_cb'),
        'dcd_event_handler': ('xfer_isr', 'sof'),
    }
    # tud_task_ext's other indirect calls: a vendor request's control callback, and a deferred function, of which
    # TinyUSB has one, msc_device.c's.
    TASK_EXTRAS = {'tud_vendor_control_xfer_cb', 'proc_async_io_done'}

    def driver_table(self) -> list[tuple[str, set[str], dict[str, str]]]:
        """Each _usbd_driver entry: its name, the configuration names its #if tests, and its fields' functions."""
        text = self.USBD.read_text(encoding='utf-8')
        table = text[text.index('_usbd_driver[] = {'):]
        table = table[:table.index('\n};')]
        entries = []
        for guard, body in re.findall(r'#if\s+([^\n]+)\n(.*?)#endif', table, re.S):
            name = re.search(r'DRIVER_NAME\("([^"]+)"\)', body).group(1)
            fields = {field: value for field, value in re.findall(r'\.(\w+)\s*=\s*(\w+)', body)
                      if field != 'name' and value != 'NULL'}
            entries.append((name, set(re.findall(r'CFG_TUD_\w+', guard)), fields))
        return entries

    def built(self) -> list[tuple[str, set[str], dict[str, str]]]:
        """The entries whose guard tusb_config.h sets to 1 (for the player image: some under WFG_PLAYER_IMAGE)."""
        enabled = set(re.findall(r'#define\s+(CFG_TUD_\w+)\s+1\b', self.TUSB_CONFIG.read_text(encoding='utf-8')))
        return [entry for entry in self.driver_table() if entry[1] & enabled]

    def rule_for(self, caller: str) -> stack_usage.Rule:
        parsed = stack_usage.parse_rules(stack_usage.RULES.read_text(encoding='utf-8'), stack_usage.RULES.name)
        found = next((rule for rule in parsed if rule.caller.search(caller)), None)
        self.assertIsNotNone(found, f'no rule covers {caller}')
        return found

    def test_the_player_builds_the_cdc_msc_and_audio_drivers(self):
        self.assertEqual({name for name, _, _ in self.built()}, self.PLAYER_DRIVERS)

    def test_each_tinyusb_rule_names_the_callbacks_of_the_drivers_built(self):
        built = self.built()
        candidates = {function for _, _, fields in self.driver_table() for function in fields.values()} | self.TASK_EXTRAS
        for caller, fields in self.FIELDS_BY_CALLER.items():
            with self.subTest(caller=caller):
                expected = {entry[2][field] for entry in built for field in fields if field in entry[2]}
                expected |= self.TASK_EXTRAS if caller == 'tud_task_ext' else set()
                rule = self.rule_for(caller)
                self.assertTrue(rule.why.strip())
                self.assertEqual({name for name in candidates if rule.targets.search(name)}, expected)

    def test_every_field_a_built_driver_sets_is_called_by_a_listed_caller(self):
        listed = {field for fields in self.FIELDS_BY_CALLER.values() for field in fields} | {'deinit'}
        self.assertEqual({field for _, _, fields in self.built() for field in fields} - listed, set())

    def test_the_dd008_session_callbacks_are_the_ones_tx_usb_passes(self):
        session = self.rule_for('m110::Status m110::CdcMuxDeviceSession::send_frame(m110::CdcMuxFrameType, '
                                'm110::CdcMuxChannel, uint32_t, std::span<const std::byte>)')
        source = (self.REPOSITORY / 'rt1170' / 'tx_usb.cpp').read_text(encoding='utf-8')
        passed = re.search(r'session\.initialize\((\w+), (\w+), (\w+), (\w+), nullptr\)', source).groups()
        for callback in passed:
            self.assertTrue(session.targets.search(f'm110::Status waveform_generator::{{anonymous}}::{callback}(void*)'),
                            callback)
        self.assertFalse(session.targets.search('m110::Status waveform_generator::{anonymous}::respond(const char*)'))
        decoder = self.rule_for('m110::Status m110::CdcMuxStreamDecoder::finish_packet(m110::CdcMuxPacketCallback, void*)')
        self.assertTrue(decoder.targets.search('static m110::Status m110::CdcMuxDeviceSession::receive_packet(void*, '
                                               'const m110::CdcMuxDecodedPacket&)'))

    def test_a_usb_task_chain_takes_the_deepest_callback_the_rules_name(self):
        # The task's entry calls TinyUSB's device task, which reaches the class drivers through their table.
        entry = '_ZN18waveform_generator14usb_task_entryEPv'
        task = graph(node(entry, 8, name='void waveform_generator::usb_task_entry(void*)'),
                     node('tud_task_ext', 40, VENDOR), node('cdcd_xfer_cb', 24, VENDOR), node('mscd_xfer_cb', 64, VENDOR),
                     node('audiod_xfer_cb', 32, VENDOR), node('hidd_xfer_cb', 4000, VENDOR),
                     edge(entry, 'tud_task_ext'), edge('tud_task_ext', '__indirect_call', site=f'{VENDOR}:5:5'))
        parsed = stack_usage.parse_rules(stack_usage.RULES.read_text(encoding='utf-8'), stack_usage.RULES.name)
        outcome = stack_usage.check(task, ROOT, entry, 1 << 13, parsed)
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 112 B', outcome.report[0])
        self.assertIn('mscd_xfer_cb', outcome.report[-1])

    def test_the_glue_forwards_to_tinyusb_s_handler_as_the_port_passes_it(self):
        # usb_tinyusb_port.c's wrapper routes each event through the glue with the real handler (its asm label
        # __real_dcd_event_handler) and its ENDPTCOMPLETE poll; the interrupt releases the held SETUPs with the same
        # forward. The glue's rules name exactly those.
        port = (self.REPOSITORY / 'rt1170' / 'usb' / 'usb_tinyusb_port.c').read_text(encoding='utf-8')
        forward, poll = re.search(r'wfg_usb_ep0_route_event\(event, in_isr, &(\w+), &(\w+)\)', port).groups()
        real = re.search(rf'void {forward}\([^)]*\) __asm__\("__real_(\w+)"\)', port).group(1)
        self.assertEqual(re.findall(r'wfg_usb_ep0_release_setups\(&(\w+)\)', port), [forward])
        candidates = (real, poll, 'audiod_xfer_isr', 'wfg_usb_ep0_route_event', 'tud_task_ext')
        route = self.rule_for('wfg_usb_ep0_route_event')
        self.assertEqual({name for name in candidates if route.targets.search(name)}, {real, poll})
        release = self.rule_for('wfg_usb_ep0_release_setups')
        self.assertEqual({name for name in candidates if release.targets.search(name)}, {real})

    def test_a_critical_error_calls_the_handler_the_firmware_installs(self):
        source = (self.REPOSITORY / 'rt1170' / 'critical_error.cpp').read_text(encoding='utf-8')
        installed = re.search(r'set_critical_error_handler\(&(\w+)\)', source).group(1)
        rule = self.rule_for('void m110::critical_error(Status)')
        self.assertTrue(rule.targets.search(f'void waveform_generator::{installed}(m110::Status, m110::CriticalSite)'))
        self.assertFalse(rule.targets.search('void m110::{anonymous}::report_and_exit(m110::Status, m110::CriticalSite)'))

    def test_a_usb_task_chain_through_the_wrapped_handler_takes_the_glue_s_frames(self):
        # The MSC driver's completion, queued from the task, goes to the wrapper (dcd_event_handler is wrapped), the
        # glue's router (72 B) and through its forward to TinyUSB's handler, which calls the audio driver's xfer_isr.
        entry = '_ZN18waveform_generator14usb_task_entryEPv'
        msc = '\n'.join((node(entry, 8, name='void waveform_generator::usb_task_entry(void*)'), node('mscd_xfer_cb', 64, VENDOR),
                         node('dcd_event_handler'), edge(entry, 'mscd_xfer_cb'), edge('mscd_xfer_cb', 'dcd_event_handler')))
        usbd = '\n'.join((node('dcd_event_handler', 48, VENDOR), node('audiod_xfer_isr', 40, VENDOR),
                          edge('dcd_event_handler', '__indirect_call', site=f'{VENDOR}:9:9')))
        port = '\n'.join((node('*__wrap_dcd_event_handler', 0, name='wfg_wrap_dcd_event_handler'), node('ep0_completion_pending', 0),
                          node('wfg_usb_ep0_route_event', 72), edge('*__wrap_dcd_event_handler', 'wfg_usb_ep0_route_event'),
                          edge('wfg_usb_ep0_route_event', '__indirect_call', site=f'{OWNED}:54:9')))
        parsed = stack_usage.parse_rules(stack_usage.RULES.read_text(encoding='utf-8'), stack_usage.RULES.name)
        task = stack_usage.read_call_graph([msc, usbd, port], ('dcd_event_handler',))
        outcome = stack_usage.check(task, ROOT, entry, 1 << 13, parsed)
        self.assertEqual(outcome.failures, [])
        self.assertIn('deepest chain 232 B', outcome.report[0])
        self.assertEqual([line.split()[-1] for line in outcome.report[1:]],
                         ['waveform_generator::usb_task_entry(void*)', 'mscd_xfer_cb', 'wfg_wrap_dcd_event_handler',
                          'wfg_usb_ep0_route_event', 'dcd_event_handler', 'audiod_xfer_isr'])

    def test_the_link_s_wraps_are_the_ones_the_stack_checks_follow(self):
        # One list names the symbols the player link wraps, and both stack checks get it.
        text = (self.REPOSITORY / 'rt1170' / 'CMakeLists.txt').read_text(encoding='utf-8')
        symbols = re.search(r'set\(wfg_wrapped_symbols ([^)]+)\)', text).group(1).split()
        self.assertEqual(symbols, ['dcd_event_handler'])
        self.assertEqual(re.findall(r'--wrap=[^"\s]*', text), ['--wrap=${symbol}'])
        self.assertRegex(text, r'foreach\(symbol IN LISTS wfg_wrapped_symbols\)\s+target_link_options\([^)]*"LINKER:--wrap=\$\{symbol\}"\)')
        check = text[text.index('scripts/check-stack-usage.py'):]
        check = check[:check.index('VERBATIM')]
        self.assertEqual(check.count('--wrap ${wfg_wrapped_symbols}\n'), 2)

    def test_the_release_images_check_the_usb_task_beside_the_player_task(self):
        text = (self.REPOSITORY / 'rt1170' / 'CMakeLists.txt').read_text(encoding='utf-8')
        check = text[text.index('scripts/check-stack-usage.py'):]
        check = check[:check.index('VERBATIM')]
        for entry, stack in (('_ZN18waveform_generator12_GLOBAL__N_117player_task_entryEPv',
                              '.bss._ZN18waveform_generator12_GLOBAL__N_117player_task_stackE'),
                             ('_ZN18waveform_generator14usb_task_entryEPv', '.bss._ZN18waveform_generator14usb_task_stackE')):
            with self.subTest(entry=entry):
                self.assertIn(f'--entry {entry}\n', check)
                self.assertIn(f'--stack-section {stack}\n', check)

    def test_public_tone_stack_rules_are_selected_only_for_its_canonical_writable_build(self):
        text = (self.REPOSITORY / 'rt1170' / 'CMakeLists.txt').read_text(encoding='utf-8')
        selection = text[text.index('set(wfg_stack_rule_args)'):text.index('add_custom_command(TARGET',
                                                                        text.index('set(wfg_stack_rule_args)'))]
        self.assertIn('if(NOT WFG_MSC_READ_ONLY AND WFG_ENABLE_ENCODER_PLUGINS)', selection)
        self.assertIn('file(REAL_PATH "${WFG_ENCODER_PLUGIN_DIR}"', selection)
        self.assertIn('if("${wfg_encoder_plugin_real}" STREQUAL "${wfg_public_tone_plugin_real}")', selection)
        self.assertIn('examples/encoder-plugin/stack-usage-rules.json', selection)
        check = text[text.index('scripts/check-stack-usage.py'):]
        check = check[:check.index('VERBATIM')]
        self.assertEqual(check.count('${wfg_stack_rule_args}'), 2)

    def test_extra_rules_extend_defaults_and_an_unknown_plugin_target_still_fails_closed(self):
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            title = ENTRY
            call_graph = '\n'.join((
                'graph: { title: "task.cpp"',
                node(title, 10, f'{scratch}/rt1170/task.cpp', name='void task_entry(void*)'),
                node('waveform_source::WavGenerator::stop', 20, f'{scratch}/rt1170/wav.cpp',
                     name='void waveform_source::WavGenerator::stop()'),
                node('waveform_example::ToneEncoder::stop', 30, f'{scratch}/examples/tone.cpp',
                     name='virtual void waveform_example::ToneEncoder::stop()'),
                edge(title, 'waveform_source::WavGenerator::stop'),
                edge('waveform_source::WavGenerator::stop', '__indirect_call'),
                '}',
            ))
            (folder / 'task.cpp.ci').write_text(call_graph, encoding='utf-8')
            (folder / 'fw.map').write_text(FileTests.MAP, encoding='utf-8')
            (folder / 'base.json').write_text('{"indirect_calls": []}', encoding='utf-8')
            extra = self.REPOSITORY / 'examples' / 'encoder-plugin' / 'stack-usage-rules.json'
            command = [sys.executable, str(SCRIPT), '--map', str(folder / 'fw.map'), '--entry', title,
                       '--stack-section', '.bss._ZN12_GLOBAL__N_110task_stackE', '--rules', str(folder / 'base.json'),
                       '--extra-rules', str(extra), '--root', scratch, '--objects', str(folder / 'task.cpp.obj')]
            (folder / 'task.cpp.obj').touch()
            passed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)

            (folder / 'task.cpp.ci').write_text('\n'.join((
                'graph: { title: "task.cpp"',
                node(title, 10, f'{scratch}/rt1170/task.cpp', name='void task_entry(void*)'),
                node('waveform_source::WavGenerator::stop', 20, f'{scratch}/rt1170/wav.cpp',
                     name='void waveform_source::WavGenerator::stop()'),
                node('other::Encoder::stop', 30, f'{scratch}/examples/other.cpp',
                     name='virtual void other::Encoder::stop()'),
                edge(title, 'waveform_source::WavGenerator::stop'),
                edge('waveform_source::WavGenerator::stop', '__indirect_call'),
                '}',
            )), encoding='utf-8')
            failed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(failed.returncode, 1, failed.stdout + failed.stderr)
            self.assertIn('targets of its rule', failed.stderr)


class FileTests(unittest.TestCase):
    MAP = """\
Memory Configuration

Name             Origin             Length             Attributes
m_data           0x20000000         0x00040000         rw
*default*        0x00000000         0xffffffff

Linker script and memory map

 .bss._ZN12_GLOBAL__N_110task_stackE
                0x20023998     0x4000 CMakeFiles/fw.dir/player.cpp.obj
"""

    def test_the_stack_is_its_array_section_in_the_map(self):
        self.assertEqual(stack_usage.stack_bytes(self.MAP, '.bss._ZN12_GLOBAL__N_110task_stackE'), 0x4000)
        with self.assertRaises(ValueError):
            stack_usage.stack_bytes(self.MAP, '.bss.other_stack')

    def test_each_c_object_needs_its_call_graph_and_an_assembly_object_none(self):
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            (folder / 'player.cpp.ci').write_text('graph: {}\n', encoding='utf-8')
            objects = [folder / 'player.cpp.obj', folder / 'startup.S.obj']
            self.assertEqual(stack_usage.call_graph_files(objects), [folder / 'player.cpp.ci'])
            with self.assertRaises(FileNotFoundError):
                stack_usage.call_graph_files(objects + [folder / 'board.c.obj'])

    def test_the_script_exits_0_1_or_2(self):
        with tempfile.TemporaryDirectory() as scratch:
            folder = Path(scratch)
            source = f'{scratch}/rt1170/player.cpp'.replace('\\', '/')
            (folder / 'task.cpp.ci').write_text('graph: { title: "task.cpp"\n' + '\n'.join(
                (node(ENTRY, 100, source), node('a', 200, source), edge(ENTRY, 'a'))) + '\n}\n', encoding='utf-8')
            (folder / 'fw.map').write_text(self.MAP, encoding='utf-8')
            (folder / 'rules.json').write_text('{"indirect_calls": []}', encoding='utf-8')

            def script(stack_section: str, objects: str = 'task.cpp.obj') -> subprocess.CompletedProcess:
                return subprocess.run([sys.executable, str(SCRIPT), '--map', str(folder / 'fw.map'), '--entry', ENTRY,
                                       '--stack-section', stack_section, '--rules', str(folder / 'rules.json'),
                                       '--root', scratch, '--objects', str(folder / objects)],
                                      capture_output=True, text=True, check=False)

            passed = script('.bss._ZN12_GLOBAL__N_110task_stackE')
            self.assertEqual(passed.returncode, 0, passed.stderr)
            self.assertIn('Stack usage: _Z10task_entryPv: deepest chain 300 B', passed.stdout)
            (folder / 'task.cpp.ci').write_text('\n'.join((node(ENTRY, 100, source), node('p', 1, source),
                                                           edge('p', 'p'))), encoding='utf-8')
            failed = script('.bss._ZN12_GLOBAL__N_110task_stackE')
            self.assertEqual(failed.returncode, 1)
            self.assertIn('FAIL owned code recurses, through p', failed.stderr)
            self.assertEqual(script('.bss.no_such_stack').returncode, 2)
            self.assertEqual(script('.bss._ZN12_GLOBAL__N_110task_stackE', 'missing.c.obj').returncode, 2)
            (folder / 'task.cpp.ci').write_text('\n'.join((node(ENTRY, 100, source), node('a', 200, source),
                                                           edge(ENTRY, 'a'))), encoding='utf-8')
            wrapped = subprocess.run([sys.executable, str(SCRIPT), '--map', str(folder / 'fw.map'), '--entry', ENTRY,
                                      '--stack-section', '.bss._ZN12_GLOBAL__N_110task_stackE', '--rules',
                                      str(folder / 'rules.json'), '--root', scratch, '--wrap', 'a', '--objects',
                                      str(folder / 'task.cpp.obj')], capture_output=True, text=True, check=False)
            self.assertEqual(wrapped.returncode, 1, wrapped.stdout + wrapped.stderr)
            self.assertIn('FAIL --wrap=a: no .ci file defines __wrap_a', wrapped.stderr)


if __name__ == '__main__':
    unittest.main()
