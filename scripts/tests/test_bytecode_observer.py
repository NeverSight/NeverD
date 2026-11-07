#!/usr/bin/env python3
"""Independent synthetic language checks for the observation CLI."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import shutil

ENGINE = str(Path(sys.argv.pop(1)).resolve())
RECOVER = str(Path(ENGINE).with_name('neverd-bytecode'))


def operand(space, offset, size=8):
    return {'space': space, 'size': size, 'value': {'addend': offset}}


def operation(name, inputs=(), output=None):
    result = {'op': name, 'inputs': list(inputs)}
    if output is not None:
        result['output'] = output
    return result


def profile():
    r = lambda n, size=8: operand('reg', n, size)
    c = lambda n, size=8: operand('const', n, size)
    copy = lambda n, target, size=8: operation('COPY', [c(n, size)], r(target, size))
    call = operation('CALL', [c(0x10000)])
    instructions = [
        [copy(0x1000, 0), copy(6, 8)],
        [operation('LOAD', [r(0)], r(16, 1)),
         operation('INT_XOR', [r(16, 1), c(0x55, 1)], r(16, 1)),
         operation('STORE', [r(0), r(16, 1)])],
        [operation('INT_ADD', [r(0), c(1)], r(0)),
         operation('INT_SUB', [r(8), c(1)], r(8)),
         operation('INT_NOTEQUAL', [r(8), c(0)], r(17, 1)),
         operation('COND_BR', [c(1), r(17, 1)])],
        [copy(0x1000, 0), call],
        [operation('RETURN')],
        [operation('INT_EQUAL', [r(24), c(0)], r(17, 1)),
         operation('COND_BR', [c(8), r(17, 1)])],
        [copy(0x2000, 0), call],
        [copy(0x3000, 0), call],
        [copy(0x123, 24, 4)],
        [copy(2, 24), operation('CALL', [c(0x10001)])],
    ]
    return {'version': 1, 'register_bytes': 32, 'temporary_bytes': 64,
            'byte_order': 'little', 'encodings': [
                {'size': 1, 'match': [{'offset': 0, 'value': i}], 'operations': ops}
                for i, ops in enumerate(instructions, 1)]}


class Observation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='neverd-observation-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.language = profile()
        self.code = bytes([1, 2, 3, 4, 5, 6, 7, 5, 8, 5])
        self.functions = [{'entry': 0, 'end': 5, 'name': 'decode_loop'},
                          {'entry': 5, 'end': 10, 'name': 'unknown_branch'}]
        self.inputs = {'max_steps': 1000, 'max_paths': 8, 'max_visits': 8,
                       'registers': [], 'memory': [], 'calls': [
                           {'target': 0x10000, 'name': 'observe',
                            'argument_registers': [0]}]}
        for name, address, data in [('encoded', 0x1000, bytes(b ^ 0x55 for b in b'hello\0')),
                                    ('left', 0x2000, b'left\0'),
                                    ('right', 0x3000, b'right\0')]:
            path = self.root / name
            path.write_bytes(data)
            self.inputs['memory'].append({'address': address, 'path': str(path)})

    def run_engine(self, success=True):
        (self.root / 'code').write_bytes(self.code)
        for name, value in [('profile', self.language), ('functions', self.functions),
                            ('inputs', self.inputs)]:
            (self.root / name).write_text(json.dumps(value))
        out = self.root / 'result'
        out.write_text('retain previous result')
        proc = subprocess.run([ENGINE, *[str(self.root / name) for name in
                                        ('code', 'profile', 'functions', 'inputs', 'result')]],
                              capture_output=True, text=True, timeout=15)
        if success:
            self.assertEqual(proc.returncode, 0, proc.stderr)
            return json.loads(out.read_text())
        self.assertGreater(proc.returncode, 0, proc.stderr)
        self.assertEqual(out.read_text(), 'retain previous result')

    def test_computed_string_and_solver_branch(self):
        result = self.run_engine()
        first, second = result['functions']
        self.assertEqual(first['calls'][0]['arguments'][0]['string'], 'hello')
        self.assertEqual(first['stops'][0]['reason'], 'external_call')
        self.assertEqual({c['arguments'][0]['string'] for c in second['calls']},
                         {'left', 'right'})
        self.assertGreaterEqual(result['solver_queries'], 2)

    def test_missing_memory_is_not_invented(self):
        self.inputs['memory'] = []
        result = self.run_engine()
        for function in result['functions']:
            for call in function['calls']:
                self.assertNotIn('string', call['arguments'][0])

    def symbol_program(self, width=8, resolver='dlsym', name=b'readlink'):
        r = lambda n, size=width: operand('reg', n, size)
        c = lambda n, size=width: operand('const', n, size)
        instructions = [
            [operation('COPY', [c(0x2000, 8)], r(0, 8)),
             operation('CALL', [c(0x10000, 8)])],
            [operation('STORE', [r(24, 8), r(8)])],
            [operation('LOAD', [r(24, 8)], r(16)),
             operation('CALL', [c(0x10001, 8)])],
            [operation('RETURN')],
            [operation('STORE', [r(24, 8), c(0xff, 1)])],
            [operation('CALL', [c(0x10002, 8)])],
        ]
        self.language = {'version': 1, 'register_bytes': 32,
                         'byte_order': 'little', 'encodings': [
                             {'size': 1, 'match': [{'offset': 0, 'value': i}],
                              'operations': ops}
                             for i, ops in enumerate(instructions, 1)]}
        self.code = bytes([1, 2, 3, 4])
        self.functions = [{'entry': 0, 'end': len(self.code), 'name': 'symbol_user'}]
        (self.root / 'left').write_bytes(name + b'\0')
        self.inputs['registers'] = [{'offset': 24, 'bytes': 8, 'value': 0x4000}]
        self.inputs['calls'] = [
            {'target': 0x10000, 'name': resolver, 'string': {'pointer_register': 0},
             'result_register': 8, 'result_bytes': width, 'result_identity': 'symbol',
             'continuation': 'register_effects', 'memory_effect': 'none'},
            {'target': 0x10001, 'name': 'indirect_gateway',
             'callee_register': 16, 'callee_bytes': width},
            {'target': 0x10002, 'name': 'opaque', 'continuation': 'havoc',
             'preserve_registers': [{'offset': 24, 'bytes': 8}]},
        ]
        # String pointers can be 32 or 64 bits independently of the host.
        if width == 4:
            self.inputs['registers'].append({'offset': 4, 'bytes': 4, 'value': 0})

    def test_symbol_identity_survives_memory_but_not_overwrite_or_havoc(self):
        for width, resolver, name in ((8, 'dlsym', b'readlink'),
                                      (4, 'GetProcAddress', b'GetTickCount')):
            with self.subTest(width=width):
                self.symbol_program(width, resolver, name)
                f = self.run_engine()['functions'][0]
                self.assertEqual(f['calls'][1]['resolved_target']['name'], name.decode())
                self.assertTrue(f['calls'][1]['resolved_target']['may_be_null'])
                self.assertEqual(f['symbol_flows'][0]['identity']['resolver'], resolver)
                for middle in (5, 6):
                    self.code = bytes([1, 2, middle, 3, 4])
                    self.functions[0]['end'] = len(self.code)
                    calls = self.run_engine()['functions'][0]['calls']
                    self.assertNotIn('resolved_target', calls[-1])

    def test_source_evidence_maps_calls_and_pointer_stores_without_changing_execution(self):
        self.symbol_program()
        report = self.run_engine()
        bindings = [{'address': 0x10000, 'name': 'resolve_gateway'},
                    {'address': 0x10001, 'name': 'invoke_gateway'}]
        (self.root / 'bindings').write_text(json.dumps(bindings))
        output = self.root / 'mapped.c'
        args = [RECOVER, str(self.root / 'code'), '--profile', str(self.root / 'profile'),
                '--functions', str(self.root / 'functions'),
                '--bindings', str(self.root / 'bindings'),
                '--observations', str(self.root / 'result'), '-o', str(output)]
        for route in ([], ['--llvm'], ['--llvm', '--optimize']):
            p = subprocess.run(args + route, capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            source = output.read_text()
            body = source[source.index('symbol_user('):]
            self.assertIn('dlsym("readlink")', body)
            self.assertIn('store resolver result "readlink"', body)
            self.assertIn('indirect readlink()', body)
            self.assertIn('0 unmapped', source)
            if route:
                self.assertIn('observed_dlsym_readlink_status', source)
                self.assertIn('observed_readlink_value', source)
            clang = shutil.which('clang')
            self.assertIsNotNone(clang, 'Clang is required for source execution coverage')
            output.write_text(source + r'''
#include <string.h>
static unsigned calls, fail;
uint64_t resolve_gateway(void *state) {
  uint64_t result = 0x123456789abcdef0ULL;
  memcpy((char *)state + 8, &result, 8);
  ++calls;
  return fail ? 17 : 0;
}
uint64_t invoke_gateway(void *state) {
  uint64_t value;
  memcpy(&value, (char *)state + 16, 8);
  ++calls;
  return value == 0x123456789abcdef0ULL ? 0 : 23;
}
int main(void) {
  for (fail = 0; fail < 2; ++fail) {
    uint64_t slot = 9;
    uint64_t state[4] = {0, 0, 0, (uintptr_t)&slot};
    calls = 0;
    uint64_t result = symbol_user(state);
    if (result != (fail ? 17 : 0) || calls != (fail ? 1 : 2) ||
        slot != (fail ? 9 : 0x123456789abcdef0ULL)) return 1;
  }
  return 0;
}
''')
            exe = self.root / 'run'
            p = subprocess.run([clang, '-std=c11', '-O2', '-Werror', str(output),
                                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)
        # A report from another input, or a forged call address, is not mapped.
        for case in ('hash', 'pc', 'target'):
            bad = json.loads(json.dumps(report))
            if case == 'hash':
                bad['input_sha256'] = '0' * 64
            else:
                bad['functions'][0]['calls'][0][case] = '0xdead'
            (self.root / 'result').write_text(json.dumps(bad))
            output.write_text('keep existing output')
            p = subprocess.run(args + ['--llvm'], capture_output=True, text=True, timeout=60)
            self.assertNotEqual(p.returncode, 0)
            self.assertEqual(output.read_text(), 'keep existing output')

    def test_symbol_stores_in_one_instruction_keep_distinct_occurrences(self):
        self.symbol_program()
        stores = self.language['encodings'][1]['operations']
        stores.append(json.loads(json.dumps(stores[0])))
        flows = self.run_engine()['functions'][0]['symbol_flows']
        self.assertEqual(len(flows), 2)
        self.assertEqual(flows[0]['pc'], flows[1]['pc'])
        self.assertNotEqual(flows[0]['instruction_operation'],
                            flows[1]['instruction_operation'])

    def test_module_identity_is_kept_separate_from_symbol_and_numeric_value(self):
        self.symbol_program(resolver='GetProcAddress', name=b'example_api')
        (self.root / 'right').write_bytes(b'example.dll\0')
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 7}], 'operations': [
                operation('COPY', [operand('const', 0x3000)], operand('reg', 0)),
                operation('CALL', [operand('const', 0x10003)])]})
        self.code = bytes([7, 1, 2, 3, 4])
        self.functions[0]['end'] = len(self.code)
        self.inputs['calls'][0]['module_register'] = 16
        self.inputs['calls'].append({
            'target': 0x10003, 'name': 'LoadLibraryA',
            'string': {'pointer_register': 0}, 'result_register': 16,
            'result_identity': 'module', 'continuation': 'register_effects',
            'memory_effect': 'none'})
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[0]['result_identity']['kind'], 'module')
        self.assertEqual(calls[1]['result_identity']['module'], 'example.dll')
        self.assertEqual(calls[2]['resolved_target']['name'], 'example_api')
        self.assertEqual(calls[2]['resolved_target']['module'], 'example.dll')
        self.inputs['calls'][0]['result_value'] = 0
        self.run_engine(False)

    def test_explicit_effects_and_default_havoc(self):
        self.code = bytes([7, 8, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'two_queries'}]
        spec = self.inputs['calls'][0]
        spec.update(continuation='register_effects', memory_effect='none')
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual([c['arguments'][0]['string'] for c in calls], ['left', 'right'])
        self.assertEqual(calls[1]['prior_opaque_calls'], 1)
        self.assertIn('continuation_contract', calls[0])
        spec['continuation'] = 'havoc'
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[0]['arguments'][0]['string'], 'left')
        self.assertNotIn('string', calls[1]['arguments'][0])
        self.assertEqual(calls[1]['arguments'][0]['initial_string_reference'], 'right')

    def test_selector_width_does_not_require_unknown_high_bytes(self):
        self.code = bytes([9, 7, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'narrow_selector'}]
        self.inputs['calls'][0].update(selector_register=24, selector_bytes=4,
                                      selector_names={'0x123': 'example_service'})
        call = self.run_engine()['functions'][0]['calls'][0]
        self.assertEqual(call['selector'], '0x123')
        self.assertEqual(call['service_name'], 'example_service')

    def test_explicit_dispatch_follows_declared_function(self):
        self.code = bytes([10, 5, 7, 5])
        self.functions = [{'entry': 0, 'end': 2, 'name': 'caller'},
                          {'entry': 2, 'end': 4, 'name': 'callee'}]
        self.inputs['entries'] = [0]
        self.inputs['calls'].append({'target': 0x10001, 'dispatch_register': 24})
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[0]['dispatched_to'], '0x2')
        self.assertEqual(calls[1]['function'], '0x2')
        self.assertEqual(calls[1]['arguments'][0]['string'], 'left')

    def test_malformed_inputs_fail_without_overwriting(self):
        self.inputs['registers'] = [{'offset': 2**64-1, 'bytes': 8, 'value': 1}]
        self.run_engine(False)
        self.inputs['registers'] = [3]
        self.run_engine(False)
        self.inputs['registers'] = []
        self.functions.append(self.functions[0])
        self.run_engine(False)

    def test_annotation_preserves_body_and_escapes_comment_terminators(self):
        self.run_engine()
        report = json.loads((self.root / 'result').read_text())
        report['functions'][0]['calls'][0]['string'] = 'name*/injected'
        (self.root / 'result').write_text(json.dumps(report))
        source = 'uint64_t decode_loop(void *state) {\n    return 17;\n}\n'
        (self.root / 'source.c').write_text(source, newline='')
        proc = subprocess.run([ENGINE, '--annotate-c', str(self.root / 'source.c'),
                               str(self.root / 'result'), str(self.root / 'annotated.c')],
                              capture_output=True, text=True, timeout=15)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        output = (self.root / 'annotated.c').read_text()
        self.assertTrue(output.endswith(source))
        self.assertNotIn('name*/injected', output)
        self.assertIn('name* /injected', output)

    def test_big_endian_register_projection(self):
        self.language['byte_order'] = 'big'
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 11}],
            'operations': [operation('CALL', [operand('const', 0x10000)])]})
        self.code = bytes([11, 5])
        self.functions = [{'entry': 0, 'end': 2, 'name': 'big_endian'}]
        self.inputs['registers'] = [{'offset': 0, 'bytes': 8,
                                    'value': 0x1122334455667788}]
        self.inputs['calls'][0].update(selector_register=0, selector_bytes=4)
        call = self.run_engine()['functions'][0]['calls'][0]
        self.assertEqual(call['selector'], '0x11223344')

    def test_expression_display_is_bounded_before_rendering(self):
        ops = []
        for _ in range(25):
            ops.append(operation('INT_ADD', [operand('reg', 0), operand('const', 1)],
                                 operand('temp', 0)))
            ops.append(operation('INT_XOR', [operand('reg', 0), operand('temp', 0)],
                                 operand('reg', 0)))
        ops.append(operation('CALL', [operand('const', 0x10000)]))
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 11}], 'operations': ops})
        self.code = bytes([11, 5])
        self.functions = [{'entry': 0, 'end': 2, 'name': 'shared_expression'}]
        call = self.run_engine()['functions'][0]['calls'][0]
        self.assertEqual(call['arguments'][0]['value']['rendering_omitted'],
                         'expression budget')

    def test_copy_disjointness_uses_symbolic_offsets_and_path_constraints(self):
        # r24 is an unknown base. Store one byte in base+32, then copy it to
        # base+8. The absolute addresses are unknown, but non-overlap is exact.
        r = lambda n: operand('reg', n)
        c = lambda n: operand('const', n)
        ops = [operation('INT_ADD', [r(24), c(8)], r(0)),
               operation('INT_ADD', [r(24), c(32)], r(8)),
               operation('STORE', [r(8), operand('const', 0x6a, 1)]),
               operation('COPY', [c(1)], r(16)),
               operation('CALL', [c(0x10001)])]
        self.language['encodings'] += [
            {'size': 1, 'match': [{'offset': 0, 'value': 11}], 'operations': ops},
            {'size': 1, 'match': [{'offset': 0, 'value': 12}], 'operations': [
                operation('LOAD', [r(0)], operand('reg', 16, 1)),
                operation('CALL', [c(0x10000)])]}]
        self.code = bytes([11, 12, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'relative_copy'}]
        self.inputs['calls'][0]['argument_registers'] = [16]
        self.inputs['registers'] = [{'offset': 16, 'bytes': 8, 'value': 0}]
        self.inputs['calls'].append({'target': 0x10001,
                                    'continuation': 'memory_copy',
                                    'destination_register': 0, 'source_register': 8,
                                    'size_register': 16, 'require_disjoint': True})
        function = self.run_engine()['functions'][0]
        self.assertTrue(function['calls'][0]['disjoint_ranges_proved'])
        self.assertEqual(function['calls'][1]['arguments'][0]['value'], '0x6a')
        # Unknown unrelated pointers may alias; equal pointers do alias.
        saved = ops[:2]
        ops[:2] = [operation('COPY', [r(24)], r(0)),
                   operation('COPY', [r(8)], r(8))]
        self.assertEqual(self.run_engine()['functions'][0]['stops'][0]['reason'],
                         'unresolved_memory_effect')
        ops[1] = operation('COPY', [r(24)], r(8))
        self.assertEqual(self.run_engine()['functions'][0]['stops'][0]['reason'],
                         'unresolved_memory_effect')
        ops[:2] = saved
        # Multi-byte ranges: touching is disjoint, overlapping is not.
        ops[3] = operation('COPY', [c(24)], r(16))
        self.assertEqual(len(self.run_engine()['functions'][0]['calls']), 2)
        ops[3] = operation('COPY', [c(25)], r(16))
        self.assertEqual(self.run_engine()['functions'][0]['stops'][0]['reason'],
                         'unresolved_memory_effect')
        # A guard can establish disjointness for otherwise unrelated pointers.
        ops[:2] = [operation('COPY', [r(24)], r(0)),
                   operation('COPY', [r(8)], r(8))]
        ops[3] = operation('COPY', [c(1)], r(16))
        self.language['encodings'].append({
            'size': 1, 'match': [{'offset': 0, 'value': 13}], 'operations': [
                operation('INT_NOTEQUAL', [r(24), r(8)], operand('temp', 0, 1)),
                operation('COND_BR', [c(2), operand('temp', 0, 1)])]})
        self.code = bytes([13, 5, 11, 12, 5])
        self.functions[0]['end'] = 5
        report = self.run_engine()
        self.assertEqual(len(report['functions'][0]['calls']), 2)
        self.assertGreaterEqual(report['solver_queries'], 2)

    def test_memory_contracts_use_exact_ranges(self):
        def load(value, register):
            return operation('COPY', [operand('const', value)], operand('reg', register))
        self.language['encodings'] += [
            {'size': 1, 'match': [{'offset': 0, 'value': 11}], 'operations': [
                load(0x4000, 0), load(0x3000, 8), load(6, 16), load(0, 24),
                operation('CALL', [operand('const', 0x10001)])]},
            {'size': 1, 'match': [{'offset': 0, 'value': 12}], 'operations': [
                operation('CALL', [operand('const', 0x10000)])]}]
        self.code = bytes([11, 12, 5])
        self.functions = [{'entry': 0, 'end': 3, 'name': 'memory_calls'}]
        spec = {'target': 0x10001, 'continuation': 'memory_copy',
                'destination_register': 0, 'source_register': 8, 'size_register': 16,
                'result_offset_register': 24, 'return_destination': True}
        self.inputs['calls'].append(spec)
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[1]['arguments'][0]['string'], 'right')
        ops = self.language['encodings'][-2]['operations']
        for destination, count in ((0x4000, 4097), (0xffffffffffffffff, 6)):
            ops[0] = load(destination, 0)
            ops[2] = load(count, 16)
            stopped = self.run_engine()['functions'][0]
            self.assertEqual(stopped['stops'][0]['reason'], 'unresolved_memory_effect')
        ops[2] = load(6, 16)
        ops[0] = load(0x3001, 0)
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[1]['arguments'][0]['string'], 'right')
        spec['require_disjoint'] = True
        stopped = self.run_engine()['functions'][0]
        self.assertEqual(stopped['stops'][0]['reason'], 'unresolved_memory_effect')
        del spec['require_disjoint']
        ops[0] = load(0x4000, 0)
        # Fill four bytes only: the adjacent 't' and terminator must survive.
        data = self.root / 'destination'
        data.write_bytes(b'right\0')
        self.inputs['memory'].append({'address': 0x4000, 'path': str(data)})
        ops = self.language['encodings'][-2]['operations']
        ops[1] = load(ord('A'), 8)
        ops[2] = load(4, 16)
        spec['continuation'] = 'memory_fill'
        calls = self.run_engine()['functions'][0]['calls']
        self.assertEqual(calls[1]['arguments'][0]['string'], 'AAAAt')
        spec['continuation'] = 'memory_havoc'
        calls = self.run_engine()['functions'][0]['calls']
        self.assertNotIn('string', calls[1]['arguments'][0])
        self.assertEqual(calls[1]['arguments'][0]['initial_string_reference'], 'right')
        spec['return_destination'] = 'true'
        self.run_engine(success=False)
        del spec['return_destination']
        del spec['size_register']
        self.run_engine(success=False)


if __name__ == '__main__':
    unittest.main()
