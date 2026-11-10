import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.generate_mingw_header_functions import declared_functions


class MinGWHeaderFunctionTests(unittest.TestCase):
    def test_macro_expansion_keeps_following_header_declarations(self):
        with tempfile.TemporaryDirectory() as temporary:
            scratch = Path(temporary)
            directory = scratch / 'sdk'
            header = str(directory / 'math.h')
            declarations = [
                {'kind': 'VarDecl', 'name': '__imp__HUGE', 'loc': {
                    'spellingLoc': {'file': '<scratch space>', 'line': 12},
                    'expansionLoc': {'file': header, 'line': 144},
                }},
                # Clang elides the unchanged expansion file on these nodes.
                *({'kind': 'FunctionDecl', 'name': name, 'loc': {'line': line}}
                  for line, name in enumerate([
                      '__mingw_raise_matherr', '__mingw_setusermatherr', '_matherr',
                  ], 164)),
                # Spelling in a header does not make a caller's expansion a
                # function that the header itself declares.
                {'kind': 'FunctionDecl', 'name': 'caller_macro', 'loc': {
                    'spellingLoc': {'file': header, 'line': 300},
                    'expansionLoc': {'file': str(scratch / 'caller.c'), 'line': 2},
                }},
                {'kind': 'FunctionDecl', 'name': 'caller_plain', 'loc': {'line': 3}},
                {'kind': 'FunctionDecl', 'name': 'compiler_intrinsic',
                 'loc': {'file': str(scratch / 'compiler' / 'builtin.h')}},
            ]
            result = subprocess.CompletedProcess(
                [], 0, stdout=json.dumps({'inner': declarations}).encode())
            with patch('scripts.generate_mingw_header_functions.subprocess.run',
                       return_value=result):
                names = declared_functions('clang', 'x86_64-w64-mingw32',
                                           ['math.h'], scratch, str(directory))
            self.assertEqual(names, {
                '__mingw_raise_matherr', '__mingw_setusermatherr', '_matherr',
            })


if __name__ == '__main__':
    unittest.main()
