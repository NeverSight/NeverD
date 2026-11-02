#!/usr/bin/env python3
"""Exact-address unwind transport preserves public source scopes and statuses."""

import json
from pathlib import Path
import re
import sys
import tempfile

from transport_test import Client


def fixtures():
    source = Path(__file__).with_name("UnwindQueryFixtures.def").read_text()
    words = {
        name: json.loads(text)
        for name, text in re.findall(
            r'NEVERD_UNWIND_TEST_TEXT\((\w+),\s*("(?:[^"\\]|\\.)*")\)', source
        )
    }
    rows = [
        (json.loads(address), json.loads(expected))
        for address, _raw, expected in re.findall(
            r'NEVERD_UNWIND_TEST_CASE\(\s*("[^"]+"),\s*R"json\((.*?)\)json",'
            r'\s*R"json\((.*?)\)json"\)', source, re.DOTALL
        )
    ]
    invalid = [
        (json.loads(payload), json.loads(code))
        for payload, code in re.findall(
            r'NEVERD_UNWIND_TEST_INVALID\(R"json\((.*?)\)json",\s*("[^"]+")\)', source
        )
    ]
    assert words and rows and invalid
    assert len(rows) == len(re.findall(r"(?m)^NEVERD_UNWIND_TEST_CASE\s*\(", source))
    assert len(invalid) == len(re.findall(r"(?m)^NEVERD_UNWIND_TEST_INVALID\s*\(", source))
    return words, rows, invalid


def run(executable):
    words, rows, invalid = fixtures()
    operation = words["kOperation"]
    with tempfile.TemporaryDirectory(prefix=words["kDirectoryPrefix"]) as directory:
        binary = Path(directory) / words["kInputName"]
        binary.write_text(words["kInputText"])
        with Client(executable) as client:
            assert operation in client.hello["capabilities"]
            unloaded = client.call(operation, {words["kAddressField"]: rows[0][0]})
            assert unloaded["error"]["code"] == "not_loaded", unloaded
            opened = client.call("open", {"path": str(binary), "read_only": True,
                                           "analysis": False})
            assert opened["status"] == "ok", opened
            before = client.call("metadata")
            for address, expected in rows:
                result = client.call(operation, {words["kAddressField"]: address})
                assert result["status"] == "ok", result
                assert result["payload"] == expected, (address, result, expected)
                assert result["revision"] == before["revision"], result
                assert result["project_id"] == before["project_id"], result
                assert result["analysis_state"] == before["analysis_state"], result
            for payload, code in invalid:
                refused = client.call(operation, payload)
                assert refused["error"]["code"] == code, refused
                assert refused["revision"] == before["revision"], refused
            after = client.call("metadata")
            assert before["payload"] == after["payload"], (before, after)
            assert binary.read_text() == words["kInputText"]
            assert list(Path(directory).iterdir()) == [binary]


if __name__ == "__main__":
    run(sys.argv[1])
