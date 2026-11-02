#!/usr/bin/env python3
"""Real worker/CLI lineage, exact source persistence and transactional stops."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from transport_test import Client


def reply(client, operation, payload=None):
    request = client.send(operation, payload)
    return client.next(lambda m: m.get("request_id") == request
                       and m.get("type") == "response" and m.get("status") != "progress", timeout=90)


def ok(client, operation, payload=None):
    result = reply(client, operation, payload)
    assert result["status"] == "ok", result
    return result["payload"]


def cli(args, *options, code=0):
    result = subprocess.run([args.neverd, "lineage", *map(str, options)],
                            capture_output=True, timeout=90)
    assert result.returncode == code, (options, result.returncode, result.stderr.decode())
    return result.stdout


def run(args, root):
    with Client(args.worker) as client:
        assert "lineage_load" in client.hello["capabilities"], client.hello
        inputs = []
        for number in (1, 2, 3):
            binary = root / f"release-{number}.bin"
            binary.write_bytes(bytes([0xB8, number, 0, 0, 0, 0xC3]))
            inputs.append({"path": str(binary), "load_options": {
                "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": "0x1000"}})
        reports, originals = [], []
        for left, right in ((0, 1), (1, 2), (0, 2)):
            ok(client, "comparison_start", {"primary": inputs[left], "secondary": inputs[right],
                                            "options": {"discover": True}})
            path = root / f"{left + 1}-{right + 1}.nddiff"
            ok(client, "comparison_save", {"path": str(path)})
            reports.append(path)
            originals.append(path.read_bytes())
        # Native sessions are disposable; offline evidence outlives them.
        ok(client, "comparison_close")
        loaded = ok(client, "lineage_load", {"reports": list(map(str, reports))})
        assert loaded["versions"] == loaded["nodes"] == loaded["edges"] == 3, loaded
        assert loaded["conflicts"] == loaded["incomplete_reports"] == 0
        assert loaded["load_completed"] and not loaded["native_authenticated"]
        revision = loaded["revision"]
        versions = ok(client, "lineage_query", {"query": {"kind": "versions", "limit": 1}})
        assert versions["total"] == 3 and versions["next_offset"] == 1
        version = versions["items"][0]["id"]
        trace = {"kind": "trace_nodes", "version": version, "address": "0x1000", "limit": 1}
        first = ok(client, "lineage_query", {"query": trace})
        assert first["total"] == 3
        last = ok(client, "lineage_query", {"query": {**trace, "offset": 2}})
        assert last["next_offset"] is None and last["items"] != first["items"]
        edges = ok(client, "lineage_query", {"query": {**trace, "kind": "trace_edges", "limit": 10}})
        assert edges["total"] == 3  # Direct A-C survives the BFS predecessor tree.
        refused = reply(client, "lineage_query", {"query": {**trace, "max_work": 1}})
        assert refused["error"]["code"] == "resource_limited", refused
        assert ok(client, "lineage_status")["loaded_revision"] == revision
        assert ok(client, "lineage_query", {"query": trace}) == first
        saved = root / "releases.ndlineage"
        ok(client, "lineage_save", {"path": str(saved)})
        bundle = json.loads(saved.read_text())
        assert sorted(r.encode() for r in bundle["reports"]) == sorted(originals)
        sources = ok(client, "lineage_query", {"query": {"kind": "reports"}})["items"]
        exported = root / "source.nddiff"
        ok(client, "lineage_save_report", {"path": str(exported), "report": sources[0]["id"]})
        assert exported.read_bytes() in originals
        assert cli(args, "--reopen", saved, "--export-report", sources[0]["id"]) == exported.read_bytes()
        assert json.loads(cli(args, "--reopen", saved, "--list", "trace_edges", "--snapshot", version,
                              "--address", "0x1000", "--json")) == edges
        for options in (("--offset", "1"), ("--limit", "1"), ("--history=false",),
                        ("--export-report", sources[0]["id"], "--source", sources[0]["id"]),
                        ("--list", "versions", "--snapshot", version),
                        ("--query", '{"kind":"versions","revision":"stale"}'),
                        ("--query=",), ("--list=",), ("--address=",), ("--export-report=",)):
            assert cli(args, "--reopen", saved, *options, code=1) == b""
        # Failure must not publish a replacement, a partial stdout or a temp file.
        target = root / "protected.ndlineage"
        target.write_bytes(b"original target")
        assert cli(args, "--reopen", saved, "--options", '{"max_work":1}', "--save", target, code=4) == b""
        assert target.read_bytes() == b"original target"
        assert not list(root.glob("protected.ndlineage.tmp-*"))
        malformed = root / "malformed.ndlineage"
        for data in (b"{bad", b'{"reports":["\xff"]}'):
            malformed.write_bytes(data)
            rejected = reply(client, "lineage_load", {"path": str(malformed)})
            assert rejected["error"]["code"] == "invalid_json", rejected
            assert "Version graph file" in rejected["error"]["message"], rejected
            assert "frame" not in rejected["error"]["message"], rejected
            assert rejected["payload"]["source"] == "lineage_file", rejected
            assert rejected["payload"]["path"] == str(malformed), rejected
            assert ok(client, "lineage_summary")["revision"] == revision
        unreadable = reply(client, "lineage_load", {"path": str(root / "missing.ndlineage")})
        assert unreadable["error"]["code"] == "load_failed", unreadable
        assert ok(client, "lineage_summary")["revision"] == revision
        for payload in ({"reports": [str(reports[0])], "options": {"max_nodes": 1}},
                        {"path": str(saved), "unknown": True}):
            assert reply(client, "lineage_load", payload)["status"] != "ok"
            assert ok(client, "lineage_summary")["revision"] == revision
        oversized = root / "oversized.nddiff"
        with oversized.open("wb") as out:
            out.truncate(2 * 1024 * 1024 * 1024 + 1)
        assert cli(args, "--report", oversized, "--save", target, code=4) == b""
        assert target.read_bytes() == b"original target"
        refused = reply(client, "lineage_load", {"reports": [str(oversized)]})
        assert refused["error"]["code"] == "resource_limited", refused
        assert ok(client, "lineage_summary")["revision"] == revision
        oversized.unlink()
        if sys.platform.startswith("linux"):
            # A signal immediately after rename cannot relabel an already saved
            # file as cancelled when a separate stdout I/O error then occurs.
            preload = root / "late-cancel.so"
            subprocess.run([args.cc, "-shared", "-fPIC", "-x", "c", "-", "-o", str(preload), "-ldl"],
                           input=b"""#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
ssize_t write(int fd, const void *data, size_t size) {
  ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
  const char *target = getenv("NEVERD_TEST_LINEAGE_TARGET");
  if (target && getenv("NEVERD_TEST_LINEAGE_FAIL_WRITE")) {
    char descriptor[64], path[4096];
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(descriptor, path, sizeof(path) - 1);
    if (length > 0) {
      path[length] = 0;
      size_t prefix = strlen(target);
      if (!strncmp(path, target, prefix) && !strncmp(path + prefix, ".tmp-", 5)) {
        errno = ENOSPC;
        return -1;
      }
    }
  }
  return real_write(fd, data, size);
}
int rename(const char *from, const char *to) {
  int (*real_rename)(const char *, const char *) = dlsym(RTLD_NEXT, "rename");
  int result = real_rename(from, to);
  const char *target = getenv("NEVERD_TEST_LINEAGE_TARGET");
  if (!result && target && !strcmp(to, target)) raise(SIGINT);
  return result;
}
""", capture_output=True, check=True)
            # A disk write failure must unwind through temporary-file cleanup,
            # not terminate in LLVM's stream destructor before cleanup runs.
            failed_env = dict(os.environ, LD_PRELOAD=str(preload),
                              NEVERD_TEST_LINEAGE_TARGET=str(target),
                              NEVERD_TEST_LINEAGE_FAIL_WRITE="1")
            result = subprocess.run([args.neverd, "lineage", "--reopen", str(saved),
                                     "--save", str(target)], capture_output=True,
                                    env=failed_env, timeout=30)
            assert result.returncode == 1 and not result.stdout, result
            assert b"cannot write lineage output" in result.stderr, result.stderr
            assert b"LLVM ERROR" not in result.stderr, result.stderr
            assert target.read_bytes() == b"original target"
            assert not list(root.glob(target.name + ".tmp-*"))
            published = root / "late-signal.ndlineage"
            env = dict(os.environ, LD_PRELOAD=str(preload), NEVERD_TEST_LINEAGE_TARGET=str(published))
            with open("/dev/full", "wb") as full:
                result = subprocess.run([args.neverd, "lineage", "--reopen", str(saved),
                                         "--save", str(published)], stdout=full, stderr=subprocess.PIPE,
                                        env=env, timeout=30)
            assert result.returncode == 1 and b"cannot write lineage stdout" in result.stderr, result
            assert json.loads(published.read_text()) == bundle
        # Partial metadata edits retain labels from other snapshots.
        rows = ok(client, "lineage_query", {"query": {"kind": "versions"}})["items"]
        for index, row in enumerate(rows[:2]):
            changed = ok(client, "lineage_load", {"append": True, "version_updates": [
                {"id": row["id"], "label": f"Release {index + 1} 中文", "branch": "stable"}]})
            assert changed["revision"] != revision
        labelled = ok(client, "lineage_query", {"query": {"kind": "versions"}})["items"]
        assert [row["label"] for row in labelled[:2]] == ["Release 1 中文", "Release 2 中文"]
        before = ok(client, "lineage_summary")["revision"]
        duplicate = [{"id": rows[0]["id"], "label": "wrong", "branch": ""}] * 2
        assert reply(client, "lineage_load", {"append": True, "version_updates": duplicate})["status"] == "error"
        assert ok(client, "lineage_summary")["revision"] == before
        assert reply(client, "lineage_query", {"expected_lineage_revision": revision,
                                                "query": {"kind": "versions"}})["error"]["code"] == "stale_lineage"
        ok(client, "lineage_save", {"path": str(saved)})
        # This exceeds the unrelated backend JSON limit. No full source crosses IPC.
        large = root / "large.nddiff"
        large.write_bytes(originals[0] + b" " * (40 * 1024 * 1024))
        request = client.send("lineage_load", {"reports": [str(large)]})
        client.next(lambda m: m.get("request_id") == request and m.get("status") == "progress"
                    and m.get("payload", {}).get("phase") == "lineage:read", timeout=30)
        ack = ok(client, "cancel", {"request_id": request})
        assert ack["accepted"] and not ack["requires_restart"], ack
        stopped = client.next(lambda m: m.get("request_id") == request and m.get("status") != "progress", timeout=90)
        assert stopped["error"]["code"] == "cancelled" and stopped["calculation_stopped"], stopped
        assert not stopped["partial_result_retained"] and not stopped["completed_before_cancellation"]
        assert ok(client, "lineage_summary")["revision"] == before
        large_summary = ok(client, "lineage_load", {"reports": [str(large)]})
        assert large_summary["reports"] == 1 and large_summary["versions"] == 2
        large_id = ok(client, "lineage_query", {"query": {"kind": "reports"}})["items"][0]["id"]
        ok(client, "lineage_save_report", {"path": str(exported), "report": large_id})
        assert exported.read_bytes() == large.read_bytes()
        ok(client, "lineage_save", {"path": str(root / "large.ndlineage")})
        ok(client, "lineage_close")
        assert ok(client, "lineage_load", {"path": str(root / "large.ndlineage")})["revision"] == large_summary["revision"]
        assert json.loads(cli(args, "--report", large, "--json"))["revision"] == large_summary["revision"]
        # Historical stopping is source evidence, not the outcome of this load.
        historical = json.loads(originals[0])
        historical["state"], historical["stopped_phase"] = "cancelled", "measure"
        incomplete = root / "incomplete.nddiff"
        incomplete.write_text(json.dumps(historical))
        summary = ok(client, "lineage_load", {"reports": [str(incomplete)]})
        assert summary["job"]["operation_state"] == "complete" and summary["incomplete_reports"] == 1
        assert json.loads(cli(args, "--report", incomplete, "--json", code=3))["incomplete_reports"] == 1
        ok(client, "lineage_close")
        assert not ok(client, "lineage_status")["result_available"]
    check_releases(args, root, reports, originals)
    print("Lineage worker/CLI: native sources, cycles, pages, exact bytes, metadata, budgets, stale requests, cancellation and >32 MiB persistence passed")


def check_releases(args, root, reports, originals):
    with Client(args.worker) as client:
        assert "lineage_edit_releases" in client.hello["capabilities"]
        assert "lineage_edit_releases" in client.hello["lineage_cancellable_operations"]
        loaded = ok(client, "lineage_load", {"reports": list(map(str, reports))})
        version = ok(client, "lineage_query", {"query": {"kind": "versions"}})["items"][0]["id"]
        old = root / "before-release-metadata.ndlineage"
        ok(client, "lineage_save", {"path": str(old)})
        def record(identifier, parents=(), date=None):
            return {"id": identifier, "label": "Same label 中文", "branch": identifier,
                    "version": version, "published_at": date, "parents": list(parents)}
        edit = {"revision": loaded["revision"], "upsert": [
            record("merge", ["stable", "preview"], "2026-10-25T08:00:00Z"),
            record("stable", (), "2026-10-24T08:00:00Z"), record("preview")]}
        added = ok(client, "lineage_edit_releases", {"edit": edit})
        assert added["releases"] == 3 and added["versions"] == loaded["versions"]
        assert added["schema_version"] == 1 and added["document_schema_version"] == 2
        assert added["edit_completed"]
        page = ok(client, "lineage_query", {"query": {"kind": "releases", "limit": 2}})
        assert [r["id"] for r in page["items"]] == ["stable", "preview"]
        assert page["total"] == 3 and page["next_offset"] == 2
        edits = root / "release-edit.json"
        edits.write_text(json.dumps(edit), encoding="utf-8")
        saved = root / "with-release-metadata.ndlineage"
        actual = json.loads(cli(args, "--reopen", old, "--release-edit", edits,
                                "--list", "releases", "--limit", 2, "--save", saved, "--json"))
        assert actual == page, (actual, page)
        assert json.loads(saved.read_text())["schema_version"] == 2
        parents = json.loads(cli(args, "--reopen", saved, "--list", "release_parents",
                                 "--release", "merge", "--json"))
        assert {r["id"] for r in parents["items"]} == {"stable", "preview"}
        before = added["revision"]
        for invalid in ({"revision": "stale", "remove": []},
                        {"remove": ["stable"]},
                        {"parent_updates": [{"release": "stable", "add": ["merge"]}]}):
            refused = reply(client, "lineage_edit_releases", {"edit": invalid})
            assert refused["status"] == "error", refused
            assert ok(client, "lineage_summary")["revision"] == before
        rename = record("merge")
        del rename["parents"]
        rename["label"] = "Relabelled, all parents retained"
        ok(client, "lineage_edit_releases", {"edit": {"upsert": [rename]}})
        assert ok(client, "lineage_query", {"query": {"kind": "release_parents", "release": "merge"}})["total"] == 2
        # Both existing append and snapshot-label workflows retain the v2 envelope.
        ok(client, "lineage_load", {"append": True, "reports": [str(reports[0])],
            "version_updates": [{"id": version, "label": "Renamed snapshot", "branch": "analysis"}]})
        snapshot_changed = ok(client, "lineage_query", {"query": {"kind": "releases"}})
        assert snapshot_changed["total"] == 3
        assert {r["version_label"] for r in snapshot_changed["items"]} == {"Renamed snapshot"}
        final = ok(client, "lineage_edit_releases", {"edit": {
            "remove": ["stable"], "parent_updates": [{"release": "merge", "remove": ["stable"]}]}})
        assert final["releases"] == 2 and final["release_links"] == 1
        before = final["revision"]
        stopped = reply(client, "lineage_edit_releases", {"edit": {"remove": []},
                                                         "options": {"max_work": 1}})
        assert stopped["error"]["code"] == "resource_limited", stopped
        assert ok(client, "lineage_summary")["revision"] == before
        ok(client, "lineage_save", {"path": str(saved)})
        ok(client, "lineage_close")
        assert ok(client, "lineage_load", {"path": str(saved)})["revision"] == before
        source = ok(client, "lineage_query", {"query": {"kind": "reports"}})["items"][0]["id"]
        assert cli(args, "--reopen", saved, "--export-report", source) in originals
        assert reply(client, "lineage_edit_releases", {"expected_lineage_revision": loaded["revision"],
            "edit": {"remove": []}})["error"]["code"] == "stale_lineage"
    print("Release metadata: CLI/worker parity, aliases, chronology, batch parent edits, stale requests, budgets and exact source export passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        run(args, args.artifacts)
    else:
        with tempfile.TemporaryDirectory(prefix="neverd-lineage-worker-") as directory:
            run(args, Path(directory))


if __name__ == "__main__":
    main()
