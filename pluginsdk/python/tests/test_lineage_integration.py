"""Native lineage ownership, callback and persistence contracts."""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest

from neverd_plugin import Lineage, NeverDError, abi
from neverd_plugin.ffi import HostAPI


class LineageUnavailableTests(unittest.TestCase):
    def test_optional_exports_fail_before_creating_a_handle(self):
        with self.assertRaisesRegex(RuntimeError, "does not export"):
            Lineage(host=HostAPI(object()))


class LineageIntegrationTests(unittest.TestCase):
    def setUp(self):
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(path)))
        temporary = tempfile.TemporaryDirectory(prefix="neverd-lineage-python-")
        self.addCleanup(temporary.cleanup)
        sessions = []
        for number in (1, 2):
            binary = Path(temporary.name) / f"return-{number}.raw"
            binary.write_bytes(bytes([0xB8, number, 0, 0, 0, 0xC3]))
            session = self.host.call("neverd_session_create")
            self.assertTrue(session)
            sessions.append(session)
            self.assertEqual(self.host.call("neverd_session_set_load_options", session,
                b'{"loader":"binary","processor":"x86_64","platform":"sysv","base":4096}'), 0)
            self.assertEqual(self.host.call("neverd_session_load", session, str(binary).encode()), 1)
        comparison = self.host.call("neverd_comparison_create")
        try:
            self.assertEqual(self.host.call("neverd_comparison_run", comparison, *sessions,
                b'{"discover":false}', abi.ComparisonProgressCallback(), None), 1)
            self.report = self.host.owned_string("neverd_comparison_export_json", comparison)
        finally:
            self.host.call("neverd_comparison_destroy", comparison)
            for session in sessions:
                self.host.call("neverd_session_destroy", session)
        self.lineage = Lineage(host=self.host)
        self.addCleanup(self.lineage.close)
        self.lineage.load_reports([self.report])

    def test_detached_sources_pages_and_reopen_remain_exact(self):
        summary = self.lineage.summary()
        self.assertEqual(summary["versions"], 2)
        self.assertFalse(summary["native_authenticated"])
        pages = list(self.lineage.pages("versions", limit=1))
        self.assertEqual([len(p["items"]) for p in pages], [1, 1])
        self.assertNotEqual(pages[0]["items"][0]["id"], pages[1]["items"][0]["id"])
        report_id = self.lineage.query("reports")["items"][0]["id"]
        self.assertEqual(self.lineage.report(report_id), self.report)
        saved = self.lineage.export()
        self.lineage.load(saved)
        self.assertEqual(self.lineage.export(), saved)
        with self.assertRaises(NeverDError):
            self.lineage.load("{invalid")
        self.assertEqual(self.lineage.status()["operation_state"], "failed")
        self.assertEqual(self.lineage.export(), saved)

    def test_callbacks_cancel_on_owner_thread_and_retry(self):
        saved = self.lineage.export()
        owner = threading.get_ident()
        calls = []

        def progress(phase, done, total):
            calls.append((phase, threading.get_ident()))
            self.assertEqual(self.lineage.status()["operation_state"], "running")
            if phase == "lineage:commit":
                self.lineage.cancel()

        with self.assertRaises(NeverDError):
            self.lineage.load(saved, progress=progress)
        self.assertTrue(calls)
        self.assertTrue(all(thread == owner for _, thread in calls))
        self.assertEqual(self.lineage.status()["operation_state"], "cancelled")
        self.assertEqual(self.lineage.export(), saved)
        self.lineage.load(saved)
        self.assertEqual(self.lineage.summary()["nodes"], 2)

    def test_release_aliases_edits_and_parent_pages_preserve_evidence(self):
        graph = self.lineage
        version = graph.query("versions")["items"][0]["id"]
        old = graph.export()
        def release(identifier, parents=()):
            return {"id": identifier, "label": "Same label", "branch": identifier,
                    "version": version, "published_at": None, "parents": list(parents)}
        graph.edit_releases({"upsert": [release("stable"), release("preview", ["stable"])]})
        rows = graph.query("releases")["items"]
        self.assertEqual([r["id"] for r in rows], ["stable", "preview"])
        self.assertEqual([r["version"] for r in rows], [version, version])
        focused = list(graph.pages("releases", focus="stable", limit=1))
        self.assertEqual([p["offset"] for p in focused], [0, 1])
        self.assertEqual([r["id"] for p in focused for r in p["items"]],
                         ["stable", "preview"])
        changed = release("preview")
        del changed["parents"]
        changed["label"] = "New label"
        graph.edit_releases({"upsert": [changed]})
        self.assertEqual(graph.query("release_parents", release="preview")["items"][0]["id"], "stable")
        before = graph.export()
        with self.assertRaises(NeverDError):
            graph.edit_releases({"remove": ["stable"]})
        self.assertEqual(graph.export(), before)
        graph.edit_releases({"parent_updates": [{"release": "preview", "remove": ["stable"]}],
                             "remove": ["stable"]})
        self.assertEqual(graph.query("releases")["total"], 1)
        saved = graph.export()
        graph.load(saved)
        self.assertEqual(graph.export(), saved)
        self.assertEqual(json.loads(saved)["schema_version"], 2)
        self.assertEqual(json.loads(old)["schema_version"], 1)
        report_id = graph.query("reports")["items"][0]["id"]
        self.assertEqual(graph.report(report_id), self.report)

    def test_release_edit_callback_failure_preserves_collection(self):
        before = self.lineage.export()
        def fail(phase, *_):
            if phase == "lineage:release_edit":
                raise ValueError("release edit cancelled by owner")
        with self.assertRaisesRegex(ValueError, "cancelled by owner"):
            self.lineage.edit_releases({"upsert": []}, progress=fail)
        self.assertEqual(self.lineage.status()["operation_state"], "cancelled")
        self.assertEqual(self.lineage.export(), before)
        with self.assertRaisesRegex(NeverDError, "stale"):
            self.lineage.edit_releases({"revision": "stale", "upsert": []})
        self.assertEqual(self.lineage.export(), before)

    def test_missing_release_export_keeps_legacy_graph_workflows(self):
        library = ctypes.CDLL(os.environ["NEVERD_TEST_LIBNEVERD"])
        class LegacyExports:
            def __getattr__(self, name):
                if name == "neverd_lineage_edit_releases_json":
                    raise AttributeError(name)
                return getattr(library, name)
        with Lineage(host=HostAPI(LegacyExports())) as graph:
            graph.load_reports([self.report])
            before = graph.export()
            with self.assertRaisesRegex(RuntimeError, "does not export"):
                graph.edit_releases({"upsert": []})
            self.assertEqual(graph.export(), before)
            self.assertEqual(graph.query("versions")["total"], 2)

    def test_callback_exception_and_reentrant_close_do_not_escape_c(self):
        saved = self.lineage.export()

        def progress(*_):
            self.lineage.close()

        with self.assertRaisesRegex(NeverDError, "during an operation"):
            self.lineage.load(saved, progress=progress)
        self.assertEqual(self.lineage.status()["operation_state"], "cancelled")
        self.assertEqual(self.lineage.export(), saved)

    def test_revision_and_small_current_budget_are_not_bypassed(self):
        revision = self.lineage.summary()["revision"]
        version = self.lineage.query("versions")["items"][0]["id"]
        trace = self.lineage.query("trace_nodes", version=version, address="0x1000", limit=1)
        self.assertEqual(trace["total"], 2)
        with self.assertRaises(NeverDError):
            self.lineage.query("trace_nodes", version=version, address="0x1000", max_work=1)
        self.assertEqual(self.lineage.status()["operation_state"], "resource_limited")
        document = json.loads(self.lineage.export())
        document["versions"][0]["label"] = "Release 1"
        self.lineage.load(document)
        with self.assertRaisesRegex(NeverDError, "stale"):
            self.lineage.query("versions", revision=revision)
        self.assertNotEqual(self.lineage.summary()["revision"], revision)

    def test_embedded_nul_and_closed_handle_refuse_without_native_truncation(self):
        saved = self.lineage.export()
        with self.assertRaises(ValueError):
            self.lineage.load(saved + "\0invalid suffix")
        self.assertEqual(self.lineage.export(), saved)
        self.lineage.close()
        with self.assertRaisesRegex(NeverDError, "closed"):
            self.lineage.summary()

    def test_data_only_byte_patch_is_a_distinct_snapshot_and_revert_recovers_it(self):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-lineage-overlay-")
        self.addCleanup(temporary.cleanup)
        raw = Path(temporary.name) / "input.raw"
        original = bytes.fromhex("b807000000c3") + bytes(18)
        raw.write_bytes(original)
        sessions = []
        for _ in range(2):
            session = self.host.call("neverd_session_create")
            self.assertTrue(session)
            sessions.append(session)
            self.addCleanup(self.host.call, "neverd_session_destroy", session)
            self.assertEqual(self.host.call("neverd_session_set_load_options", session,
                b'{"loader":"binary","processor":"x86_64","platform":"sysv","base":4096}'), 0)
            self.assertEqual(self.host.call("neverd_session_load", session, str(raw).encode()), 1)

        def compare():
            comparison = self.host.call("neverd_comparison_create")
            try:
                self.assertEqual(self.host.call("neverd_comparison_run", comparison, *sessions,
                    b'{"discover":false}', abi.ComparisonProgressCallback(), None), 1)
                return self.host.owned_string("neverd_comparison_export_json", comparison)
            finally:
                self.host.call("neverd_comparison_destroy", comparison)

        before = compare()
        options = abi.NeverDBytePatchOptionsV1()
        options.struct_size = ctypes.sizeof(options)
        options.address = 0x1010
        options.kind = b"bytes"
        options.bytes = b"2a"
        self.assertTrue(self.host.owned_string("neverd_byte_patch_apply", sessions[0], ctypes.byref(options)))
        after = compare()
        old, new = json.loads(before), json.loads(after)
        self.assertEqual(old["primary"]["sha256"], new["primary"]["sha256"])
        self.assertEqual(old["functions"], new["functions"])
        self.assertNotEqual(old["primary"]["revision"], new["primary"]["revision"])
        self.assertEqual(raw.read_bytes(), original)
        self.lineage.load_reports([before, after])
        self.assertEqual(self.lineage.summary()["versions"], 2)
        self.assertEqual(self.lineage.summary()["nodes"], 2)
        self.assertEqual(self.host.call("neverd_byte_patch_revert", sessions[0], 0x1010, 1), 0)
        restored = compare()
        self.lineage.load_reports([before, restored])
        self.assertEqual(self.lineage.summary()["versions"], 1)


if __name__ == "__main__":
    unittest.main()
