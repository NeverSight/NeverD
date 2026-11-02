#!/usr/bin/env python3
"""Owned ELF import contracts through CLI, C API and worker; no input execution."""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(arguments, expected=(0, 2, 3)):
    result = subprocess.run(arguments, text=True, capture_output=True, timeout=60)
    assert result.returncode in expected, (arguments, result.returncode, result.stderr, result.stdout)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--worker")
    args = parser.parse_args()
    source = Path(__file__).with_name("call_effects_fixture.c")
    with tempfile.TemporaryDirectory(prefix="neverd-diff-effects-") as folder:
        root = Path(folder)
        inputs = [root / "before.so", root / "after.so"]
        for path, result in zip(inputs, (17, 9)):
            run([args.compiler, "-shared", "-nostdlib", "-fPIC", "-O1", "-fno-builtin",
                 "-fno-asynchronous-unwind-tables", "-fno-stack-protector",
                 f"-DPATCH_RESULT={result}", str(source), "-o", str(path)], (0,))
        base = [args.neverd, "diff", "--structural", "-a", str(inputs[0]), "-b", str(inputs[1])]
        inventory = json.loads(run(base + ["--list", "all", "--limit", "200"]).stdout)
        entry = next(row["address"] for row in inventory["items"] if row["name"] == "effect_entry")
        flags = ["--impact-function", entry, "--impact-direction", "callees", "--impact-kind", "effects"]
        missing = json.loads(run(base + flags).stdout)
        assert missing["effect_review"]["qualified_contracts"] == 0
        assert any(row["effect"]["status"] == "ssa_unavailable" for row in missing["rows"])
        saved = root / "effects.nddiff"
        page = json.loads(run(base + flags + ["--dataflow", "--save", str(saved)]).stdout)
        assert page["state"] == "complete", page
        assert page["effect_review"]["profile"] == "native-import-effects-v1"
        assert page["effect_review"]["qualified_contracts"] == 2, page
        assert page["effect_review"]["data_propagation"] == "not_performed"
        assert page["effect_review"]["must_release"] == "not_proved"
        assert page["semantic_validation"] == "not_performed"
        contracts = {row["callee"]["name"]: row for row in page["rows"]
                     if row["effect"]["status"] == "qualified_contract"}
        assert set(contracts) == {"memcpy", "free"}, contracts
        assert contracts["memcpy"]["effect"]["argument_roles"]["length"] == 2
        assert contracts["memcpy"]["effect"]["argument_roles"]["capacity"] is None
        assert contracts["free"]["effect"]["argument_roles"]["handle"] == 0
        assert all(row["instruction"]["bytes"] and row["effect"]["ssa_node"] is not None
                   and "memory_alias" in row["effect"]["ssa_gaps"] for row in contracts.values())
        assert any(row["callee"]["name"] == "project_release" and
                   row["effect"]["status"] == "unsupported_contract" for row in page["rows"])
        assert json.loads(run(base + flags + ["--reopen", str(saved)]).stdout) == page
        limited = json.loads(run(base + flags + ["--dataflow", "--effect-max-records", "1"], (3,)).stdout)
        assert limited["state"] == "resource_limited" and len(limited["rows"]) == 1
        run(base + ["--effect-max-work", "1"], (1,))
        run(base + flags + ["--effect-max-work", "0"], (1,))

        lib = ctypes.CDLL(args.library)
        ptr = ctypes.c_void_p
        for name in ("neverd_session_create", "neverd_comparison_create"):
            getattr(lib, name).restype = ptr
        for name in ("neverd_session_destroy", "neverd_comparison_destroy", "neverd_free_string"):
            getattr(lib, name).argtypes = [ptr]
        lib.neverd_session_load.argtypes = [ptr, ctypes.c_char_p]
        lib.neverd_comparison_run.argtypes = [ptr, ptr, ptr, ctypes.c_char_p, ptr, ptr]
        lib.neverd_comparison_impact_json.argtypes = [ptr, ctypes.c_char_p]
        lib.neverd_comparison_impact_json.restype = ptr
        a, b, comparison = lib.neverd_session_create(), lib.neverd_session_create(), lib.neverd_comparison_create()
        query = {"seeds": [entry], "direction": "callees", "kind": "effects"}

        def api_query(q):
            text = lib.neverd_comparison_impact_json(comparison, json.dumps(q).encode())
            assert text, q
            try:
                return json.loads(ctypes.string_at(text))
            finally:
                lib.neverd_free_string(text)

        try:
            assert lib.neverd_session_load(a, str(inputs[0]).encode()) == 1
            assert lib.neverd_session_load(b, str(inputs[1]).encode()) == 1
            assert lib.neverd_comparison_run(comparison, a, b, b'{"dataflow":true}', None, None) == 1
            lib.neverd_session_destroy(a)
            lib.neverd_session_destroy(b)
            a = b = None
            assert api_query(query) == page
            second = api_query({**query, "offset": 1, "limit": 1})
            assert second["rows"] == page["rows"][1:2] and second["effect_review"] == page["effect_review"]
            stopped_query = {**query, "max_effect_work": 1}
            stopped = api_query(stopped_query)
            assert stopped["state"] == "resource_limited" and not stopped["rows"]
            assert api_query(stopped_query) == stopped
            assert api_query({**query, "refresh": True}) == page
        finally:
            lib.neverd_comparison_destroy(comparison)
            if a:
                lib.neverd_session_destroy(a)
            if b:
                lib.neverd_session_destroy(b)

        if args.worker:
            sys.path.insert(0, str(source.resolve().parents[2] / "tools/neverd-worker/tests"))
            from transport_test import Client

            def worker_query(client, operation, payload):
                reply = client.call(operation, payload)
                assert reply["status"] == "ok", reply
                return reply["payload"]

            with Client(args.worker) as client:
                start = {"primary": {"path": str(inputs[0])}, "secondary": {"path": str(inputs[1])},
                         "options": {"dataflow": True}}
                worker_query(client, "comparison_start", start)
                output = worker_query(client, "comparison_impact", query)
                assert output["comparison_revision"]
                assert all(output[key] == value for key, value in page.items())
                worker_query(client, "comparison_start", {**start, "restore_path": str(saved)})
                reopened = worker_query(client, "comparison_impact", query)
                assert all(reopened[key] == value for key, value in page.items())
                stopped = worker_query(client, "comparison_impact", stopped_query)
                assert stopped["state"] == "resource_limited"
                assert worker_query(client, "comparison_impact", stopped_query) == stopped
                refreshed = worker_query(client, "comparison_impact", {**query, "refresh": True})
                assert refreshed["rows"] == page["rows"]
                assert client.call("comparison_impact", {**query, "max_effect_records": 0})["status"] == "error"
        else:
            print("SKIP: worker parity (worker target unavailable)")
        print("ELF import contracts: CLI/C API/reopen/paging/bounds verified; no target execution")


if __name__ == "__main__":
    main()
