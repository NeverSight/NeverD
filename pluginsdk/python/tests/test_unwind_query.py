"""Exact-address unwind queries preserve the complete native JSON response."""

import json
from pathlib import Path
import re
import unittest
from unittest import mock

from neverd_plugin import NeverDError, Session
from neverd_plugin.abi import UNWIND_FRAME_QUERY


class NativeBridge:
    def session_address(self, handle):
        return handle


def fixture_source():
    return (Path(__file__).parent / "fixtures" / "UnwindQueryCases.def").read_text()


class UnwindQueryTests(unittest.TestCase):
    def test_exact_addresses_preserve_all_native_results(self):
        source = fixture_source()
        cases = re.findall(
            r'^NEVERD_PY_UNWIND_CASE\(\s*(\d+),\s*R"json\((.*?)\)json"\)',
            source,
            re.MULTILINE,
        )
        self.assertEqual(len(cases), source.count("\nNEVERD_PY_UNWIND_CASE("))
        self.assertTrue(cases)
        for address, response in cases:
            with self.subTest(address=address, response=response):
                host = mock.Mock()
                host.owned_string.return_value = response
                session = Session(id(host), _native=NativeBridge(), _host=host)
                self.assertEqual(session.unwind_frame(int(address)), json.loads(response))
                self.assertEqual(host.owned_string.call_count, 1)
                args = host.owned_string.call_args.args
                self.assertEqual(args[0], UNWIND_FRAME_QUERY)
                self.assertEqual(args[1].value, id(host))
                self.assertEqual(args[2:], (int(address),))
                host.call.assert_not_called()

    def test_invalid_addresses_do_not_enter_the_native_query(self):
        source = fixture_source()
        cases = re.findall(
            r"^NEVERD_PY_UNWIND_INVALID\((.*)\)$", source, re.MULTILINE
        )
        self.assertEqual(len(cases), source.count("\nNEVERD_PY_UNWIND_INVALID("))
        self.assertTrue(cases)
        host = mock.Mock()
        session = Session(id(host), _native=NativeBridge(), _host=host)
        for text in cases:
            value = json.loads(text)
            error = ValueError if type(value) is int else TypeError
            with self.subTest(value=value), self.assertRaises(error):
                session.unwind_frame(value)
        host.owned_string.assert_not_called()

    def test_missing_and_malformed_results_are_not_json_null(self):
        words = {
            name: json.loads(text)
            for name, text in re.findall(
                r'^NEVERD_PY_UNWIND_TEXT\((\w+),\s*("(?:[^"\\]|\\.)*")\)$',
                fixture_source(),
                re.MULTILINE,
            )
        }
        host = mock.Mock()
        session = Session(id(host), _native=NativeBridge(), _host=host)
        host.owned_string.return_value = None
        with self.assertRaises(NeverDError):
            session.unwind_frame(0)
        host.owned_string.return_value = words["MALFORMED_JSON"]
        with self.assertRaises(NeverDError):
            session.unwind_frame(0)
        host.owned_string.return_value = words["JSON_NULL"]
        self.assertIsNone(session.unwind_frame(0))

    def test_session_lifetime_is_checked_before_querying(self):
        native = mock.Mock()
        native.session_address.side_effect = RuntimeError()
        host = mock.Mock()
        session = Session(id(host), _native=native, _host=host)
        with self.assertRaises(RuntimeError):
            session.unwind_frame(0)
        host.owned_string.assert_not_called()
