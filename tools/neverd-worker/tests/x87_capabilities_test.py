#!/usr/bin/env python3
"""Compare worker inspection with the shared C, Python and optional CLI report."""

import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from transport_test import Client


def run(args):
    root = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(root / "pluginsdk/python"))
    from neverd_plugin.api import Session
    from neverd_plugin.ffi import HostAPI

    # Borrow a real C session for the public Python API, without pretending
    # that this standalone test is an embedded Python plugin.
    class BorrowedSession:
        def session_address(self, handle):
            return handle

    host = HostAPI(ctypes.CDLL(str(Path(args.library).resolve())))
    handle = host.call("neverd_session_create")
    assert handle
    try:
        report = json.loads(host.owned_string("neverd_x87_capabilities_json", handle))
        session = Session(handle, _native=BorrowedSession(), _host=host)
        assert session.x87_capabilities() == report
        assert report["schema_version"] == 1
        assert report["forms"] and report["coverage"] and report["source_paths"]
        if args.cli:
            original = subprocess.run([args.cli, "x87-capabilities"], check=True,
                                      capture_output=True, text=True)
            assert json.loads(original.stdout) == report

        with tempfile.TemporaryDirectory(prefix="neverd-x87-report-") as directory:
            binary = Path(directory) / "inspection.evm"
            contents = "0x600160020100"
            binary.write_text(contents, encoding="ascii")
            with Client(args.worker) as client:
                assert "x87_capabilities" in client.hello["capabilities"]
                initial = client.call("x87_capabilities")
                assert initial["status"] == "ok", initial
                assert initial["payload"] == report
                assert initial["project_id"] == "" and initial["revision"] == "0"
                assert client.call("metadata")["error"]["code"] == "not_loaded"
                for payload in ({"probe_host": True}, {"profile": "invented"}):
                    refused = client.call("x87_capabilities", payload)
                    assert refused["error"]["code"] == "invalid_request", refused
                    assert refused["revision"] == "0"
                opened = client.call("open", {"path": str(binary),
                                              "read_only": True, "analysis": False})
                assert opened["status"] == "ok", opened
                before = client.call("metadata")
                queried = client.call("x87_capabilities")
                after = client.call("metadata")
                assert queried["status"] == "ok", queried
                assert queried["payload"] == report
                assert queried["revision"] == before["revision"] == after["revision"]
                assert queried["project_id"] == before["project_id"] == after["project_id"]
                assert before["payload"] == after["payload"]
                assert binary.read_text(encoding="ascii") == contents
            if args.legacy_worker:
                with Client(args.legacy_worker) as legacy:
                    assert "x87_capabilities" not in legacy.hello["capabilities"]
                    unavailable = legacy.call("x87_capabilities")
                    assert unavailable["error"]["code"] == "unsupported", unavailable
                    assert unavailable["revision"] == "0"
    finally:
        host.call("neverd_session_destroy", handle)
    print("x87 inventory: worker/C/Python parity, optional CLI parity, read-only query and legacy refusal passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker")
    parser.add_argument("library")
    parser.add_argument("--cli")
    parser.add_argument("--legacy-worker")
    run(parser.parse_args())
