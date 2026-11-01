"""Compiler comparisons may ignore visibility, never parameter ABI differences."""

from pathlib import Path
import argparse
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_microsoft_provider_abis as provider


class MicrosoftProviderABIs(unittest.TestCase):
    def test_generated_declarations_do_not_inherit_the_wdk_default_calling_convention(self):
        compiler = ['clang', '--target=i686-pc-windows-msvc', '-I', 'wdk',
                    '-ivfsoverlay', 'headers.json', '-Xclang', '-fdefault-calling-conv=stdcall',
                    '-Xclang', '-fms-compatibility', '-ffreestanding']
        self.assertEqual(provider.generated_compiler(compiler),
                         ['clang', '--target=i686-pc-windows-msvc', '-Xclang',
                          '-fms-compatibility', '-ffreestanding'])
        self.assertEqual(provider.generated_compiler(compiler, keep_sdk_headers=True),
                         ['clang', '--target=i686-pc-windows-msvc', '-I', 'wdk',
                          '-ivfsoverlay', 'headers.json', '-Xclang', '-fms-compatibility',
                          '-ffreestanding'])

    def test_visibility_is_independent_of_call_and_record_contracts(self):
        ir = ('@neverd_ucrt_abi_0 = global ptr @"\\01_routine@8", align 4\n'
              'declare dllimport x86_stdcallcc zeroext i8 @"\\01_routine@8"'
              '(ptr byval(%struct.Pair) align 4) #0\n')
        expected = provider.compiler_contracts(ir, ("routine",))
        for visibility in ('dso_local ', ''):
            self.assertEqual(provider.compiler_contracts(ir.replace('dllimport ', visibility),
                                                         ("routine",)), expected)
        for before, after in [('x86_stdcallcc ', ''), ('zeroext ', ''),
                              ('align 4)', 'align 8)'), ('@8', '@4')]:
            self.assertNotEqual(provider.compiler_contracts(ir.replace(before, after),
                                                            ("routine",)), expected)


class NdisProviderABIs(unittest.TestCase):
    def check_profile(self, *, problems=(), prototypes=None, changed=None, missing=False):
        names = ["NdisCloseConfiguration", "NdisInlineRoutine"]
        inventory = names + ["OtherRoutine"] if prototypes is None else prototypes
        ir = ''.join(f'@neverd_ucrt_abi_{i} = global ptr @neverd_ndis_contract_{i}, align 4\n'
                     f'declare x86_stdcallcc void @neverd_ndis_contract_{i}(ptr)\n'
                     for i in range(len(names)))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / 'catalog'
            (generated / 'x86').mkdir(parents=True)
            original = root / 'original.c'
            original.write_text('/* original WDK profile */\n')
            header = generated / 'WindowsKernelX86.h'
            header.write_text('void NdisCloseConfiguration(void *);\n')
            command = ['clang', '--target=i686-pc-windows-msvc', '-I', 'official-headers',
                       '-Xclang', '-fdefault-calling-conv=stdcall', '-E', str(original)]
            library = {'name': 'WindowsKernelX86', 'sha256': provider.digest(header),
                       'providers': [], 'commands': [command]}
            (generated / 'manifest.json').write_text('{}')
            (generated / 'x86/manifest.json').write_text(json.dumps({'libraries': [library]}))
            args = argparse.Namespace(generated=generated, output=root / 'result',
                                      neverd='neverd', architecture=['x86'],
                                      provider=['NDIS'], timeout=10)
            invocations, sources = [], []

            def run(command, **kwargs):
                invocations.append(command)
                text, code = '', 0
                if '-E' in command:
                    text = 'void NdisCloseConfiguration(void *);\n'
                elif '--json' in command:
                    text = json.dumps({'problems': list(problems), 'prototypes': inventory})
                elif '--merge-prototypes' in command:
                    if missing:
                        code = 1
                    else:
                        Path(command[command.index('-o') + 1]).write_text('/* selected */\n')
                else:
                    self.assertIn('-emit-llvm', command)
                    source = Path(command[-3])
                    sources.append((source.name, source.read_text(), command))
                    text = changed if source.name == 'generated.c' and changed is not None else ir
                return subprocess.CompletedProcess(command, code, stdout=text,
                                                   stderr='missing published declaration' if code else '')

            with mock.patch.object(provider.subprocess, 'run', side_effect=run):
                report = provider.check(args)
            retained = json.loads((args.output / 'report.json').read_text())
            self.assertEqual(json.loads(json.dumps(report)), retained)
            return retained, invocations, sources

    def test_original_inventory_selects_every_function_and_compares_inline_types(self):
        report, commands, sources = self.check_profile()
        self.assertTrue(report['passed'])
        row, = report['cases']
        self.assertEqual(row['functions'], ['NdisCloseConfiguration', 'NdisInlineRoutine'])
        selection, = [c for c in commands if '--merge-prototypes' in c]
        self.assertIn('--import-prototype=NdisInlineRoutine', selection)
        self.assertNotIn('--import-prototype=OtherRoutine', selection)
        self.assertEqual(len(sources), 2)
        for name, text, command in sources:
            self.assertIn('extern __typeof__(NdisInlineRoutine) neverd_ndis_contract_1;', text)
            self.assertIn('neverd_ucrt_abi_1 = &neverd_ndis_contract_1;', text)
            self.assertEqual('-fdefault-calling-conv=stdcall' in command, name == 'original.c')
            self.assertEqual('-I' in command, name == 'original.c')

    def test_missing_published_function_is_a_failure_not_a_smaller_inventory(self):
        report, commands, _ = self.check_profile(missing=True)
        self.assertFalse(report['passed'])
        self.assertEqual(len(report['cases'][0]['functions']), 2)
        selection, = [c for c in commands if '--merge-prototypes' in c]
        self.assertIn('--import-prototype=NdisInlineRoutine', selection)
        self.assertTrue(report['cases'][0]['error'])

    def test_empty_or_refused_official_inventory_cannot_pass(self):
        for settings in ({'prototypes': ['OtherRoutine']}, {'problems': ['bad WDK declaration']}):
            with self.subTest(settings=settings):
                report, commands, _ = self.check_profile(**settings)
                self.assertFalse(report['passed'])
                self.assertEqual(len(commands), 2)
                self.assertIn('official NDIS', report['cases'][0]['error'])

    def test_aliases_retain_parameter_return_and_calling_convention_contracts(self):
        prefix = '@neverd_ucrt_abi_0 = global ptr @neverd_ndis_contract_0, align 4\n'
        suffix = ('@neverd_ucrt_abi_1 = global ptr @neverd_ndis_contract_1, align 4\n'
                  'declare x86_stdcallcc void @neverd_ndis_contract_1(ptr)\n')
        for changed in ('declare void @neverd_ndis_contract_0(ptr)\n',
                        'declare x86_stdcallcc i64 @neverd_ndis_contract_0(ptr)\n',
                        'declare x86_stdcallcc void @neverd_ndis_contract_0(i32)\n'):
            with self.subTest(changed=changed):
                report, _, _ = self.check_profile(changed=prefix + changed + suffix)
                self.assertFalse(report['passed'])
                self.assertEqual(report['cases'][0]['error'],
                                 'original and generated compiler ABIs differ')


if __name__ == "__main__":
    unittest.main()
