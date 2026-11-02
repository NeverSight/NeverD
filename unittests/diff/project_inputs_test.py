#!/usr/bin/env python3
"""NDDB inputs across the public SDK, CLI and worker; no target execution."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zlib


def pack(path, data, name="owned.bin", base="0x1000", chunk_size=128 * 1024):
    with sqlite3.connect(path) as db:
        db.executescript("""
            PRAGMA application_id = 1313096770;
            CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE blobs(name TEXT NOT NULL, chunk INTEGER NOT NULL, size INTEGER NOT NULL,
                               data BLOB NOT NULL, PRIMARY KEY(name,chunk));
            CREATE TABLE sidecars(suffix TEXT PRIMARY KEY, data BLOB NOT NULL);
            CREATE TABLE state(key TEXT PRIMARY KEY, value BLOB NOT NULL);
        """)
        db.executemany("INSERT INTO meta VALUES(?,?)", [
            ("format", "1"), ("input_name", name), ("input_size", str(len(data))),
            ("input_sha256", hashlib.sha256(data).hexdigest()),
            ("generator", "independent Python SQLite/zlib fixture")])
        for index, offset in enumerate(range(0, len(data), chunk_size)):
            piece = data[offset:offset + chunk_size]
            encoded = struct.pack(">I", len(piece)) + zlib.compress(piece)
            db.execute("INSERT INTO blobs VALUES('input',?,?,?)", (index, len(piece), encoded))
        db.executemany("INSERT INTO sidecars VALUES(?,?)", [
            (".neverd-load.json", json.dumps({"loader": "binary", "processor": "x86_64",
                                             "platform": "sysv", "base": base}).encode()),
            (".neverd-renames.json", json.dumps([{"addr": base, "renamed": "saved_check"}]).encode()),
            (".neverd-types.json", json.dumps([{"language": "c", "source": "typedef unsigned int saved_word;"}]).encode()),
            (".neverd-debug.json", json.dumps({"pdb": {"mode": "none"},
                                              "dwarf": {"mode": "none"},
                                              "map": {"mode": "none"}}).encode()),
        ])


def mutate(path, sql, parameters=()):
    with sqlite3.connect(path) as db:
        db.execute(sql, parameters)


def run(arguments, expected=(0, 2, 3)):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=60)
    assert result.returncode in expected, (result.returncode, result.stdout, result.stderr)
    return result


def sdk_checks(library, root, a, b, a_bytes):
    lib = ctypes.CDLL(library)
    pointer = ctypes.c_void_p
    lib.neverd_session_create.restype = pointer
    lib.neverd_session_destroy.argtypes = [pointer]
    lib.neverd_session_load.argtypes = [pointer, ctypes.c_char_p]
    lib.neverd_session_load_cancellable.argtypes = [pointer, ctypes.c_char_p, pointer, pointer]
    lib.neverd_session_set_load_progress.argtypes = [pointer, pointer, pointer]
    lib.neverd_session_set_load_options.argtypes = [pointer, ctypes.c_char_p]
    lib.neverd_free_string.argtypes = [pointer]
    for name in ("neverd_last_error", "neverd_session_file_path", "neverd_session_input_sha256",
                 "neverd_type_declarations_json", "neverd_renames_json", "neverd_annotations_json"):
        getattr(lib, name).argtypes = [pointer]
        getattr(lib, name).restype = pointer
    for name in ("neverd_renames_load", "neverd_annotations_load"):
        getattr(lib, name).argtypes = [pointer]
    lib.neverd_comparison_create.restype = pointer
    lib.neverd_comparison_destroy.argtypes = [pointer]
    lib.neverd_comparison_run_owned.argtypes = [pointer, ctypes.POINTER(pointer), ctypes.POINTER(pointer), ctypes.c_char_p, pointer, pointer]
    lib.neverd_comparison_export_json.argtypes = [pointer]
    lib.neverd_comparison_export_json.restype = pointer

    def text(name, session):
        value = getattr(lib, name)(session)
        assert value, name
        try:
            return ctypes.string_at(value).decode()
        finally:
            lib.neverd_free_string(value)

    first, second, comparison = pointer(lib.neverd_session_create()), None, None
    try:
        assert lib.neverd_session_load(first, os.fsencode(a)) == 1, text("neverd_last_error", first)
        extracted = Path(text("neverd_session_file_path", first))
        temporary = extracted.parent
        assert extracted.read_bytes() == a_bytes
        assert temporary.parent == root / "temporary", temporary
        assert text("neverd_session_input_sha256", first) == hashlib.sha256(a_bytes).hexdigest()
        declarations = json.loads(text("neverd_type_declarations_json", first))
        assert declarations and "saved_word" in declarations[0]["source"], declarations
        # The returned binary remains session-owned even if a caller reloads it.
        assert lib.neverd_session_load(first, os.fsencode(extracted)) == 1
        assert extracted.exists()
        # Invalid direct sidecar reloads also retain complete prior metadata.
        private = Path(str(extracted) + ".neverd")
        renamed = private / "renames.json"
        original_renames = renamed.read_bytes()
        known = text("neverd_renames_json", first)
        renamed.write_bytes(b'[{"addr":"0x1000","renamed":"partial"},{"addr":"bad"}]')
        assert lib.neverd_renames_load(first) == -1
        assert text("neverd_renames_json", first) == known
        renamed.write_bytes(original_renames)
        notes = private / "annotations.json"
        notes.write_bytes(b'[{"addr":"0x1000","text":"retained"}]')
        assert lib.neverd_annotations_load(first) == 0
        known = text("neverd_annotations_json", first)
        notes.write_bytes(b'[{"addr":"0x1000","text":"partial"},{"addr":"invalid","text":"bad"}]')
        assert lib.neverd_annotations_load(first) == 1
        assert text("neverd_annotations_json", first) == known
        notes.unlink()
        assert lib.neverd_annotations_load(first) == 0
        cases = [
            ("foreign", "PRAGMA application_id=1196444487", ()),
            ("future", "UPDATE meta SET value='2' WHERE key='format'", ()),
            ("negative", "UPDATE meta SET value='-1' WHERE key='input_size'", ()),
            ("size", "UPDATE meta SET value='99999' WHERE key='input_size'", ()),
            ("escape", "UPDATE meta SET value='../escape.bin' WHERE key='input_name'", ()),
            ("nul", "UPDATE meta SET value=? WHERE key='input_name'", ("bad\0name",)),
            ("digest", "UPDATE meta SET value=? WHERE key='input_sha256'", ("0" * 64,)),
            ("gap", "UPDATE blobs SET chunk=8 WHERE chunk=0", ()),
            ("bad-zlib", "UPDATE blobs SET data=X'00000006ffff'", ()),
            ("trailing", "UPDATE blobs SET data=CAST(data || X'00' AS BLOB)", ()),
            ("chunk-size", "UPDATE blobs SET size=3", ()),
            ("sidecar-path", "INSERT INTO sidecars VALUES('../outside',X'5b5d')", ()),
            ("sidecar-json", "UPDATE sidecars SET data=X'7b' WHERE suffix='.neverd-renames.json'", ()),
            ("rename-row", "UPDATE sidecars SET data=? WHERE suffix='.neverd-renames.json'",
             (b'[{"addr":"0x1000","wrong":"lost name"}]',)),
            ("annotation-row", "INSERT INTO sidecars VALUES('.neverd-annotations.json',?)",
             (b'[{"addr":"invalid","text":"lost note"}]',)),
            ("function-row", "INSERT INTO sidecars VALUES('.neverd-functions.json',?)",
             (b'[{"addr":"nonsense","state":"created"}]',)),
            ("types-row", "UPDATE sidecars SET data=? WHERE suffix='.neverd-types.json'",
             (b'[{"language":17,"source":"bad"}]',)),
            ("debug-missing", "UPDATE sidecars SET data=? WHERE suffix='.neverd-debug.json'",
             (json.dumps({"pdb": {"path": str(root / "absent.pdb"), "mode": "verified"}}).encode(),)),
            ("debug-mode-type", "UPDATE sidecars SET data=? WHERE suffix='.neverd-debug.json'",
             (b'{"pdb":{"mode":false}}',)),
            ("debug-mode", "UPDATE sidecars SET data=? WHERE suffix='.neverd-debug.json'",
             (b'{"pdb":{"mode":"guessed"}}',)),
        ]
        for name, sql, values in cases:
            damaged = root / f"damaged-{name}.nddb"
            shutil.copyfile(a, damaged)
            mutate(damaged, sql, values)
            before = set((root / "temporary").iterdir())
            assert lib.neverd_session_load(first, os.fsencode(damaged)) == 0, name
            assert text("neverd_last_error", first), name
            assert Path(text("neverd_session_file_path", first)) == extracted, name
            assert extracted.read_bytes() == a_bytes, name
            assert set((root / "temporary").iterdir()) == before, name

        large = root / "large.nddb"
        pack(large, b"\xcc" * (3 * 1024 * 1024))
        progress_type = ctypes.CFUNCTYPE(None, pointer, ctypes.c_char_p, ctypes.c_ulonglong,
                                        ctypes.c_ulonglong, ctypes.c_char_p)
        stop_type = ctypes.CFUNCTYPE(ctypes.c_int, pointer)
        stopped = False

        @progress_type
        def progress(user, phase, done, total, detail):
            nonlocal stopped
            if phase == b"project-unpack" and done > 0:
                stopped = True

        @stop_type
        def cancel(user):
            return int(stopped)

        before = set((root / "temporary").iterdir())
        lib.neverd_session_set_load_progress(first, progress, None)
        assert lib.neverd_session_load_cancellable(first, os.fsencode(large), cancel, None) == -2
        lib.neverd_session_set_load_progress(first, None, None)
        assert stopped and extracted.exists()
        assert set((root / "temporary").iterdir()) == before
        assert Path(text("neverd_session_file_path", first)) == extracted

        # An explicit caller placement takes precedence over the packed choice.
        placed = pointer(lib.neverd_session_create())
        try:
            assert lib.neverd_session_set_load_options(placed, b'{"loader":"binary","processor":"x86_64","platform":"sysv","base":"0x8000"}') == 0
            assert lib.neverd_session_load(placed, os.fsencode(a)) == 1
            lib.neverd_session_base_addr.argtypes = [pointer]
            lib.neverd_session_base_addr.restype = ctypes.c_uint64
            assert lib.neverd_session_base_addr(placed) == 0x8000
        finally:
            lib.neverd_session_destroy(placed)
        # A replacement owns a different workspace and retires the previous one.
        assert lib.neverd_session_load(first, os.fsencode(a)) == 1
        assert not temporary.exists()
        first_folder = Path(text("neverd_session_file_path", first)).parent
        second = pointer(lib.neverd_session_create())
        assert lib.neverd_session_load(second, os.fsencode(b)) == 1
        second_folder = Path(text("neverd_session_file_path", second)).parent
        comparison = lib.neverd_comparison_create()
        result = lib.neverd_comparison_run_owned(comparison, ctypes.byref(first), ctypes.byref(second), b"{}", None, None)
        assert result == 1, result
        assert not first.value and not second.value
        first = second = None
        assert first_folder.exists() and second_folder.exists()
        value = lib.neverd_comparison_export_json(comparison)
        assert value
        try:
            report = json.loads(ctypes.string_at(value))
        finally:
            lib.neverd_free_string(value)
        assert report["primary"]["path"] == str(a)
        assert report["primary"]["sha256"] == hashlib.sha256(a_bytes).hexdigest()
        assert report["primary"]["project"]["input_name"] == "旧版本.bin"
        lib.neverd_comparison_destroy(comparison)
        comparison = None
        assert not first_folder.exists() and not second_folder.exists()
    finally:
        if comparison:
            lib.neverd_comparison_destroy(comparison)
        if first:
            lib.neverd_session_destroy(first)
        if second:
            lib.neverd_session_destroy(second)


def cli_checks(executable, root, a, b):
    base = [executable, "diff", "--structural", "-a", str(a), "-b", str(b)]
    saved = root / "review.nddiff"
    report = json.loads(run(base + ["--json", "--save", str(saved)]).stdout)
    assert report["primary"]["path"] == str(a)
    assert report["primary"]["project"]["input_name"] == "旧版本.bin"
    assert report["primary"]["format"] == "raw"
    reopened = json.loads(run(base + ["--json", "--reopen", str(saved)]).stdout)
    assert reopened == report
    for field in ("input_name", "state_sha256"):
        altered = json.loads(json.dumps(report))
        altered["primary"]["project"][field] = "tampered.bin" if field == "input_name" else "1" * 64
        tampered = root / f"tampered-{field}.nddiff"
        tampered.write_text(json.dumps(altered))
        run(base + ["--json", "--reopen", str(tampered)], (1,))
    rows = json.loads(run(base + ["--list", "all"]).stdout)["items"]
    assert any(row["name"] == "saved_check" for row in rows), rows
    detail = json.loads(run(base + ["--func", "saved_check", "--json"]).stdout)
    assert detail["rows"]
    proof = json.loads(run(base + ["--validate-function", "0x1000"]).stdout)
    assert proof["status"] == "unproved" and not proof["unproved_means_inequivalent"], proof
    # Both byte identity and project assertions bind a saved review. A comment
    # or type-only save must not replay evidence from a different project state.
    changed = root / "changed.nddb"
    shutil.copyfile(a, changed)
    mutate(changed, "UPDATE sidecars SET data=? WHERE suffix='.neverd-types.json'",
           (b'[{"language":"c","source":"typedef unsigned long saved_word;"}]',))
    mismatch = run([executable, "diff", "--structural", "-a", str(changed), "-b", str(b),
                    "--json", "--reopen", str(saved)], (1,))
    assert "revision" in mismatch.stderr.lower() or "input" in mismatch.stderr.lower(), mismatch.stderr
    # A saved project can be compared with a normal file using its own sidecar.
    raw = root / "plain.bin"
    raw.write_bytes(bytes.fromhex("b809000000c3"))
    sidecars = Path(str(raw) + ".neverd")
    sidecars.mkdir()
    (sidecars / "load.json").write_text(json.dumps({"loader": "binary", "processor": "x86_64",
                                                  "platform": "sysv", "base": "0x1000"}))
    mixed = json.loads(run([executable, "diff", "--structural", "-a", str(a), "-b", str(raw), "--json"]).stdout)
    assert "project" in mixed["primary"] and "project" not in mixed["secondary"]
    # Legacy unstamped projects and renamed, stamped projects remain readable.
    legacy = root / "legacy.nddb"
    shutil.copyfile(a, legacy)
    mutate(legacy, "PRAGMA application_id=0")
    run([executable, "diff", "--structural", "-a", str(legacy), "-b", str(b), "--json"])
    renamed = root / "renamed.data"
    shutil.copyfile(a, renamed)
    run([executable, "diff", "--structural", "-a", str(renamed), "-b", str(b), "--json"])


def worker_checks(executable, root, a, b):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/neverd-worker/tests"))
    from transport_test import Client

    def ok(client, operation, payload=None):
        response = client.call(operation, payload)
        assert response["status"] == "ok", response
        return response["payload"]

    with Client(executable) as client:
        summary = ok(client, "comparison_start", {"primary": {"path": str(a)},
                                                  "secondary": {"path": str(b)}})
        revision = summary["comparison_revision"]
        assert summary["primary"]["path"] == str(a)
        ok(client, "comparison_validate", {"expected_comparison_revision": revision})
        inventory = ok(client, "comparison_functions", {"expected_comparison_revision": revision})
        assert any(row["name"] == "saved_check" for row in inventory["items"])
        bad = client.call("comparison_start", {"primary": {"path": str(root / "damaged-digest.nddb")},
                                                "secondary": {"path": str(b)}})
        assert bad["status"] == "error", bad
        assert ok(client, "comparison_summary")["comparison_revision"] == revision
        large = root / "worker-large.nddb"
        pack(large, b"\xcc" * (64 * 1024 * 1024))
        for side in ("primary", "secondary"):
            start = {"primary": {"path": str(a)}, "secondary": {"path": str(b)}}
            start[side] = {"path": str(large)}
            request = client.send("comparison_start", start)
            progress = client.next(lambda message: message.get("request_id") == request
                                   and message.get("status") == "progress"
                                   and message.get("payload", {}).get("phase") == f"{side}:load:project-unpack")
            assert progress["payload"]["done"] < progress["payload"]["total"]
            ok(client, "cancel", {"request_id": request})
            stopped = client.response(request)
            assert stopped["status"] == "error" and stopped["error"]["code"] == "cancelled", stopped
            assert ok(client, "comparison_summary")["comparison_revision"] == revision
        # Keep a WAL writer connected. Its commits leave the main file alone.
        wal_path = root / "wal.nddb"
        shutil.copyfile(a, wal_path)
        with sqlite3.connect(wal_path) as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA wal_autocheckpoint=0")
            db.execute("UPDATE meta SET value='before' WHERE key='generator'")
            db.commit()
            summary = ok(client, "comparison_start", {"primary": {"path": str(wal_path)},
                                                       "secondary": {"path": str(b)}})
            stamp = (wal_path.stat().st_mtime_ns, wal_path.stat().st_size)
            db.execute("UPDATE sidecars SET data=? WHERE suffix='.neverd-renames.json'",
                       (b'[{"addr":"0x1000","renamed":"updated_in_wal"}]',))
            db.commit()
            assert (wal_path.stat().st_mtime_ns, wal_path.stat().st_size) == stamp
            stale = client.call("comparison_functions", {
                "expected_comparison_revision": summary["comparison_revision"]})
            assert stale["status"] == "error" and stale["error"]["code"] == "stale_input", stale


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--worker")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="neverd-project-inputs-") as folder:
        root = Path(folder)
        (root / "temporary").mkdir()
        os.environ["TMPDIR"] = str(root / "temporary")
        os.environ["TEMP"] = str(root / "temporary")
        os.environ["TMP"] = str(root / "temporary")
        a, b = root / "before.nddb", root / "after.nddb"
        a_bytes = bytes.fromhex("b811000000c3")
        pack(a, a_bytes, "旧版本.bin")
        pack(b, bytes.fromhex("b809000000c3"), "新版本.bin")
        originals = a.read_bytes(), b.read_bytes()
        sdk_checks(args.library, root, a, b, a_bytes)
        cli_checks(args.neverd, root, a, b)
        if args.worker:
            worker_checks(args.worker, root, a, b)
        assert (a.read_bytes(), b.read_bytes()) == originals
        assert not list((root / "temporary").iterdir())
    print("NDDB SDK/CLI/worker integrity, settings, lifetime, reopen and stale-WAL checks passed")


if __name__ == "__main__":
    main()
