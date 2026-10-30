"""Compiler comparisons may ignore visibility, never parameter ABI differences."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_microsoft_provider_abis as provider


class MicrosoftProviderABIs(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
