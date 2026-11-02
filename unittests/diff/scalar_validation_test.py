#!/usr/bin/env python3
"""Owned compiled scalar models through CLI/C API/worker; no target execution."""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, expected=(0, 2, 3)):
    result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    assert result.returncode in expected, (command, result.returncode, result.stderr, result.stdout)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--worker")
    args = parser.parse_args()
    source = Path(__file__).with_name("scalar_validation_fixture.c")
    with tempfile.TemporaryDirectory(prefix="neverd-scalar-validation-") as folder:
        root = Path(folder)
        a, b, changed = (root / name for name in ("before.so", "after.so", "changed.so"))
        for path, level, patch in ((a, "-O1", 0), (b, "-O2", 0), (changed, "-O2", 1)):
            run([args.compiler, "-shared", "-nostdlib", "-fPIC", level, "-fno-builtin",
                 "-funwind-tables" if level == "-O2" else "-fno-asynchronous-unwind-tables",
                 "-fno-stack-protector",
                 f"-DPATCH_RESULT={patch}", str(source), "-o", str(path)], (0,))
        base = [args.neverd, "diff", "--structural", "-a", str(a), "-b", str(b)]
        inventory = json.loads(run(base + ["--list", "all"]).stdout)
        entries = {row["name"]: row["address"] for row in inventory["items"]}
        address = entries["scalar_entry"]
        flags = ["--validate-function", address]
        saved = root / "comparison.nddiff"
        result = json.loads(run(base + flags + ["--save", str(saved)]).stdout)
        assert result["status"] == "proved", result
        assert result["input_domain"] == "defined_bitvectors"
        assert result["inputs_bound"] and result["arguments"]
        assert result["input_source_binding"] == "owned_sessions"
        assert not result["inputs_revalidated"]
        assert result["native_state_equivalence"] == "not_proved"
        assert result["lifting_correctness"] == "assumed"
        assert result["target_execution"] == "not_performed"
        assert not result["persistent_certificate"]
        assert result == json.loads(run(base + flags + ["--reopen", str(saved)]).stdout)
        assert "scalar_validation" not in saved.read_text()
        memory = json.loads(run(base + ["--validate-function", entries["scalar_memory"]]).stdout)
        assert memory["status"] == "unsupported", memory
        limited = json.loads(run(base + flags + ["--validation-max-work", "1"], (3,)).stdout)
        assert limited["state"] == "resource_limited" and limited["status"] == "budget_exceeded"
        run(base + ["--validation-max-work", "1"], (1,))
        run(base + flags + ["--validation-max-work", "0"], (1,))
        different = json.loads(run([args.neverd, "diff", "--structural", "-a", str(a),
                                   "-b", str(changed)] + flags, (3,)).stdout)
        assert different["status"] == "unproved", different
        assert not different["unproved_means_inequivalent"]

        unwind = root / "with-unwind.so"
        run([args.compiler, "-shared", "-nostdlib", "-fPIC", "-O2", "-fno-builtin",
             "-funwind-tables", "-fno-stack-protector", str(source), "-o", str(unwind)], (0,))
        unwind_base = [args.neverd, "diff", "--structural", "-a", str(unwind), "-b", str(unwind)]
        unwind_rows = json.loads(run(unwind_base + ["--list", "all"]).stdout)["items"]
        unwind_address = next(row["address"] for row in unwind_rows if row["name"] == "scalar_entry")
        unwind_result = json.loads(run(unwind_base + ["--validate-function", unwind_address]).stdout)
        assert unwind_result["status"] == "proved", unwind_result
        assert unwind_result["exception_metadata"] == "absent_or_canonical_source_unwind_only"

        lib = ctypes.CDLL(args.library)
        ptr = ctypes.c_void_p
        for name in ("neverd_session_create", "neverd_comparison_create"):
            getattr(lib, name).restype = ptr
        for name in ("neverd_session_destroy", "neverd_comparison_destroy", "neverd_free_string"):
            getattr(lib, name).argtypes = [ptr]
        lib.neverd_session_load.argtypes = [ptr, ctypes.c_char_p]
        lib.neverd_comparison_run_owned.argtypes = [ptr, ctypes.POINTER(ptr), ctypes.POINTER(ptr), ctypes.c_char_p, ptr, ptr]
        lib.neverd_comparison_owned_scalar_validation_json.argtypes = [ptr, ctypes.c_char_p]
        lib.neverd_comparison_owned_scalar_validation_json.restype = ptr
        left, right = ptr(lib.neverd_session_create()), ptr(lib.neverd_session_create())
        comparison = lib.neverd_comparison_create()
        query = {"address": address}
        try:
            assert lib.neverd_session_load(left, str(a).encode()) == 1
            assert lib.neverd_session_load(right, str(b).encode()) == 1
            assert lib.neverd_comparison_run_owned(comparison, ctypes.byref(left), ctypes.byref(right), b"{}", None, None) == 1
            assert not left.value and not right.value
            text = lib.neverd_comparison_owned_scalar_validation_json(comparison, json.dumps(query).encode())
            assert text
            try:
                assert json.loads(ctypes.string_at(text)) == result
            finally:
                lib.neverd_free_string(text)
        finally:
            lib.neverd_comparison_destroy(comparison)
            lib.neverd_session_destroy(left)
            lib.neverd_session_destroy(right)

        if args.worker:
            sys.path.insert(0, str(source.resolve().parents[2] / "tools/neverd-worker/tests"))
            from transport_test import Client

            def request(client, operation, payload):
                reply = client.call(operation, payload)
                assert reply["status"] == "ok", reply
                return reply["payload"]

            client = Client(args.worker)
            try:
                assert "comparison_scalar_validation" in client.hello["capabilities"]
                assert "comparison_scalar_validation" in client.hello["comparison_cancellable_operations"]
                start = {"primary": {"path": str(a)}, "secondary": {"path": str(b)}}
                request(client, "comparison_start", start)
                for payload in (start, {**start, "restore_path": str(saved)}):
                    request(client, "comparison_start", payload)
                    actual = request(client, "comparison_scalar_validation", query)
                    actual.pop("comparison_revision")
                    assert actual == result, actual
                failed = client.call("comparison_start", {**start, "options": {"unknown_option": True}})
                assert failed["status"] == "error", failed
                actual = request(client, "comparison_scalar_validation", query)
                actual.pop("comparison_revision")
                assert actual == result, actual
                request(client, "comparison_close", {})
                assert client.call("comparison_scalar_validation", query)["status"] == "error"
            finally:
                client.close()
        else:
            print("SKIP: worker parity (worker target unavailable)")
        print("Scalar return validation: defined input domain, pure compiled models, canonical unwind-only records, change/memory refusal, "
              "limits, SDK/CLI/worker and fresh reopen verified; no target execution")


if __name__ == "__main__":
    main()
