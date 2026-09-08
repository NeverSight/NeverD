"""Swift source workflow, coverage accounting and backend process contracts."""
from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))
from mobile.common import Limits, MobileError
from mobile.swift_source import _coverage, recover_swift_sources


SYMBOL = "$s4Demo6answerys5Int32VF"
SOURCE = "public func answer() -> Int32 { return 42 }\n"


def inventory(*, unsupported=False, metadata=False, unknown=False):
    methods = [{"entry": "0x1000", "mangled_symbol": SYMBOL, "status": "supported",
                "classification": "callable", "name": "answer", "parameters": []}]
    if unsupported:
        methods.append({"entry": "0x1100", "mangled_symbol": "$s4Demo5asyncyyYaF",
                        "status": "unsupported", "classification": "callable", "reason": "async signature"})
    symbols = []
    if metadata:
        symbols.append({"entry": "0x1200", "mangled_symbol": "$s4Demo6RecordVMn",
                        "status": "unsupported", "classification": "metadata", "reason": "not a function"})
    if unknown:
        symbols.append({"entry": "0x1300", "mangled_symbol": "$s4DemoUnknown",
                        "status": "unsupported", "classification": "unknown", "reason": "unknown node"})
    return {"schema_version": 1, "methods": methods, "symbols": symbols,
            "method_count": len(methods), "symbol_count": len(methods) + len(symbols),
            "unclassified_symbol_count": int(unknown), "supported_signature_count": 1,
            "unsupported_signature_count": int(unsupported), "logs": [], "limitations": ["signature limitation"]}


def source_batch(*, unsupported=False):
    rows = [{"entry": "0x1000", "mangled_symbol": SYMBOL, "status": "recovered", "source": SOURCE}]
    if unsupported:
        rows.append({"entry": "0x1100", "mangled_symbol": "$s4Demo5asyncyyYaF",
                     "status": "unrecovered", "reason": "async signature"})
    return {"schema_version": 1, "status": "success", "source": SOURCE + "\n",
            "method_count": len(rows), "recovered_method_count": 1,
            "unrecovered_method_count": len(rows) - 1, "methods": rows,
            "coverage_status": "partial" if unsupported else "recovered",
            "source_units": [{"kind": "function", "module": "Demo", "name": "answer",
                              "source": SOURCE, "method_entries": ["0x1000"],
                              "method_identities": [{"entry": "0x1000", "mangled_symbol": SYMBOL}]}], "types": [],
            "limitations": ["backend limitation"]}


def compiler_batch():
    declared, batch = inventory(), source_batch()
    runtime = {"entry": "0x1100", "mangled_symbol": "$s4Demo3BoxCMa",
               "status": "unsupported", "classification": "callable",
               "node_kind": "TypeMetadataAccessFunction", "module": "Demo",
               "context_kind": "class", "context_name": "Box", "name": "typeMetadata",
               "declaration_kind": "runtime", "runtime_source_kind": "type_metadata_accessor",
               "requires_runtime_source_proof": True, "reason": "native proof required"}
    declared["methods"].append(runtime)
    declared.update(method_count=2, symbol_count=2, unsupported_signature_count=1)
    source = "class Box { public func answer() -> Int32 { return 42 } }\n"
    batch["methods"].append({"entry": runtime["entry"], "mangled_symbol": runtime["mangled_symbol"],
                             "status": "recovered", "source": source,
                             "source_representation": "compiler-generated-from-type",
                             "compiler_projection_kind": "type_metadata_accessor",
                             "compiler_projection_evidence": ["Exact native metadata lookup and return state."]})
    batch.update(source=source + "\n", method_count=2, recovered_method_count=2,
                 source_body_method_count=1, compiler_projection_method_count=1)
    batch["source_units"] = [{"kind": "type", "module": "Demo", "name": "Box", "source": source,
                              "method_entries": ["0x1000", "0x1100"],
                              "method_identities": [{key: row[key] for key in ("entry", "mangled_symbol")}
                                                    for row in batch["methods"]]}]
    return declared, batch


class SwiftSourceWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="neverd swift workflow ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in ("sources", "metadata", "artifacts", "logs"):
            (self.root / name).mkdir()
        self.binary = self.root / "artifacts/selected.macho"
        self.binary.write_bytes(b"selected slice")
        self.env = patch.dict(os.environ, {}, clear=True)
        self.env.start()
        self.addCleanup(self.env.stop)

    def recover(self, **kwargs):
        options = {"symbols": [{"name": SYMBOL, "address": "0x1000"}], "neverd": "native tool",
                   "demangler": None, "pointer_size": 8, "max_func": 7, "limits": Limits()}
        options.update(kwargs)
        return recover_swift_sources(self.binary, self.root, **options)

    def test_process_uses_selected_binary_and_keeps_real_source_and_coverage(self):
        declared = inventory(unsupported=True, metadata=True, unknown=True)
        declared["logs"] = ["swift-demangle-0000.log"]

        def backend(argv, log, timeout):
            self.assertEqual(argv[:4], ["native tool", "export", str(self.binary), "--format=swift-methods"])
            signatures = Path(argv[4].split("=", 1)[1])
            self.assertEqual(json.loads(signatures.read_text()), declared)
            self.assertIn("--max-func=7", argv)
            self.assertEqual(timeout, 300)
            Path(argv[argv.index("-o") + 1]).write_text(json.dumps(source_batch(unsupported=True)))
            log.write_text("complete")

        with patch("mobile.swift_source.shutil.which", return_value="/tool with spaces/swift-demangle"), \
                patch("mobile.swift_source.recover_swift_signatures", return_value=declared) as recover, \
                patch("mobile.swift_source.run_tool", side_effect=backend):
            report, outputs = self.recover()
        self.assertEqual(recover.call_args.kwargs["demangler"], "/tool with spaces/swift-demangle")
        self.assertEqual(report["status"], "partial")
        self.assertEqual(report["method_count"], 2)
        self.assertEqual(report["metadata_symbol_count"], 1)
        self.assertEqual(report["unclassified_symbol_count"], 1)
        self.assertEqual(report["unrecovered_method_count"], 1)
        self.assertEqual(report["non_method_symbols"][0]["status"], "not-callable")
        self.assertNotIn("reason", report["non_method_symbols"][0])
        self.assertEqual(report["methods"][1]["reason"], "async signature")
        self.assertNotIn("source", report["methods"][0])
        self.assertEqual((self.root / outputs["swift_source"]).read_text(), SOURCE + "\n")
        self.assertEqual(json.loads((self.root / outputs["swift_method_coverage"]).read_text()), report)
        self.assertFalse((self.root / "artifacts/swift-recovery.json").exists())
        self.assertEqual(outputs["swift_demangle_logs"], ["logs/swift-demangle-0000.log"])

    def test_unknown_symbols_prevent_complete_coverage_claim(self):
        report = _coverage(inventory(unknown=True), source_batch())
        self.assertEqual(report["coverage_status"], "recovered")
        self.assertEqual(report["status"], "partial")
        self.assertEqual(report["unclassified_symbol_count"], 1)

    def test_compiler_projections_preserve_evidence_and_separate_body_counts(self):
        declared, batch = compiler_batch()
        report = _coverage(declared, batch)
        self.assertEqual(report["recovered_method_count"], 2)
        self.assertEqual(report["source_body_method_count"], 1)
        self.assertEqual(report["compiler_projection_method_count"], 1)
        self.assertEqual(report["methods"][0]["source_representation"], "native-method-body")
        runtime = report["methods"][1]
        self.assertEqual(runtime["signature_status"], "unsupported")
        self.assertEqual(runtime["status"], "recovered")
        self.assertEqual(runtime["source_representation"], "compiler-generated-from-type")
        self.assertEqual(runtime["compiler_projection_kind"], "type_metadata_accessor")
        self.assertEqual(runtime["compiler_projection_evidence"], batch["methods"][1]["compiler_projection_evidence"])
        self.assertNotIn("source", runtime)
        self.assertEqual(len(report["source_units"][0]["method_identities"]), 2)

    def test_compiler_projection_cannot_hide_missing_proof_or_belong_to_another_type(self):
        for case in range(8):
            declared, batch = compiler_batch()
            row = batch["methods"][1]
            if case == 0:
                row.pop("source_representation")
            elif case == 1:
                row["compiler_projection_kind"] = "modify_accessor"
            elif case == 2:
                row["compiler_projection_evidence"] = []
            elif case == 3:
                row["compiler_projection_evidence"] = ["invalid\0evidence"]
            elif case == 4:
                batch["source_body_method_count"] = 2
            elif case == 5:
                batch["source_units"][0]["name"] = "DifferentType"
            elif case == 6:
                row["source"] = "class Box {}\n"
            else:
                batch["source_units"][0]["kind"] = "function"
            with self.subTest(case=case), self.assertRaises(MobileError):
                _coverage(declared, batch)

    def test_ordinary_method_cannot_acquire_compiler_status(self):
        batch = source_batch()
        batch["methods"][0]["source_representation"] = "compiler-generated-from-type"
        with self.assertRaisesRegex(MobileError, "ordinary method"):
            _coverage(inventory(), batch)

    def test_type_unit_groups_methods_and_type_metadata_does_not_change_coverage(self):
        batch = source_batch()
        declaration = "public final class Calculator { public func answer() -> Int32 { return 42 } }\n"
        batch["source"] = declaration + "\n"
        batch["source_units"] = [{"kind": "type", "module": "Demo", "name": "Calculator",
                                  "source": declaration, "method_entries": ["0x1000"],
                                  "method_identities": [{"entry": "0x1000", "mangled_symbol": SYMBOL}]}]
        batch["types"] = [{"name": "Calculator", "status": "partial"}]
        report = _coverage(inventory(), batch)
        self.assertEqual(report["method_count"], 1)
        self.assertEqual(report["source_type_count"], 1)
        self.assertEqual(report["type_metadata_count"], 1)
        self.assertEqual(report["types"], batch["types"])
        self.assertNotIn("source", report["source_units"][0])

    def test_source_units_cannot_omit_duplicate_or_invent_recovered_methods(self):
        for entries in ([], ["0x1000", "0x1000"], ["0x1100"], ["0x1000", "0x1100"]):
            batch = source_batch(unsupported=True)
            batch["source_units"][0]["method_entries"] = entries
            with self.subTest(entries=entries), self.assertRaisesRegex(MobileError, "uniquely cover"):
                _coverage(inventory(unsupported=True), batch)

    def test_distinct_symbols_with_shared_entry_are_separate_recovered_methods(self):
        declared, batch = inventory(), source_batch()
        alias = "$s4Demo6secondys5Int64VF"
        declared["methods"].append({**declared["methods"][0], "mangled_symbol": alias, "name": "second"})
        declared.update(method_count=2, symbol_count=2, supported_signature_count=2)
        batch["methods"].append({**batch["methods"][0], "mangled_symbol": alias})
        batch.update(method_count=2, recovered_method_count=2)
        unit = batch["source_units"][0]
        unit["method_entries"].append("0x1000")
        unit["method_identities"].append({"entry": "0x1000", "mangled_symbol": alias})
        report = _coverage(declared, batch)
        self.assertEqual(report["status"], "recovered")
        self.assertEqual(report["recovered_method_count"], 2)
        self.assertEqual(report["source_units"][0]["method_entries"], ["0x1000", "0x1000"])
        self.assertEqual([row["mangled_symbol"] for row in report["methods"]], [SYMBOL, alias])

    def test_source_unit_identity_cannot_be_unknown_missing_or_duplicate(self):
        valid = {"entry": "0x1000", "mangled_symbol": SYMBOL}
        for identities in ([], [valid, valid], [{**valid, "mangled_symbol": "$s4DemoUnknown"}]):
            batch = source_batch()
            batch["source_units"][0]["method_identities"] = identities
            batch["source_units"][0]["method_entries"] = [row["entry"] for row in identities]
            with self.subTest(identities=identities), self.assertRaisesRegex(MobileError, "uniquely cover"):
                _coverage(inventory(), batch)
        declared, batch = inventory(), source_batch()
        declared["methods"].append(deepcopy(declared["methods"][0]))
        declared.update(method_count=2, symbol_count=2, supported_signature_count=2)
        batch["methods"].append(deepcopy(batch["methods"][0]))
        batch.update(method_count=2, recovered_method_count=2)
        batch["source_units"][0]["method_identities"] = [valid, valid]
        batch["source_units"][0]["method_entries"] = ["0x1000", "0x1000"]
        with self.assertRaisesRegex(MobileError, "uniquely cover"):
            _coverage(declared, batch)

    def test_no_symbols_or_32_bit_slice_do_not_invoke_tools(self):
        with patch("mobile.swift_source.shutil.which") as which, patch("mobile.swift_source.run_tool") as run:
            report, _ = self.recover(symbols=[], demangler="nonexistent")
            self.assertEqual(report["status"], "no-symbols")
            report, _ = self.recover(pointer_size=4)
            self.assertEqual(report["status"], "unsupported-architecture")
            self.assertEqual(report["unclassified_symbol_count"], 1)
            which.assert_not_called()
            run.assert_not_called()

    def test_auto_missing_demangler_preserves_unknown_symbol_inventory(self):
        with patch("mobile.swift_source.shutil.which", return_value=None), patch("mobile.swift_source.run_tool") as run:
            report, outputs = self.recover()
        self.assertEqual(report["status"], "unavailable")
        self.assertEqual(report["method_count"], 0)
        self.assertEqual(report["unclassified_symbol_count"], 1)
        self.assertNotIn("swift_source", outputs)
        self.assertEqual(report["non_method_symbols"][0]["mangled_symbol"], SYMBOL)
        run.assert_not_called()

    def test_explicit_and_environment_demangler_fail_clearly(self):
        with patch("mobile.swift_source.shutil.which", return_value=None):
            with self.assertRaisesRegex(MobileError, "configured Swift demangler"):
                self.recover(demangler="/missing/swift-demangle")
            with patch.dict(os.environ, {"NEVERD_SWIFT_DEMANGLE": "/missing/env-demangler"}):
                with self.assertRaisesRegex(MobileError, "configured Swift demangler"):
                    self.recover()

    def test_explicit_tool_precedes_environment_and_metadata_is_not_a_method(self):
        declared = inventory(metadata=True)
        declared.update(methods=[], method_count=0, symbol_count=1, supported_signature_count=0)
        with patch.dict(os.environ, {"NEVERD_SWIFT_DEMANGLE": "environment-tool"}), \
                patch("mobile.swift_source.shutil.which", return_value="/explicit") as which, \
                patch("mobile.swift_source.recover_swift_signatures", return_value=declared), \
                patch("mobile.swift_source.run_tool") as run:
            report, outputs = self.recover(demangler="explicit-tool")
        which.assert_called_once_with("explicit-tool")
        run.assert_not_called()
        self.assertEqual(report["status"], "no-methods")
        self.assertEqual(report["metadata_symbol_count"], 1)
        self.assertEqual(report["method_count"], 0)
        self.assertNotIn("swift_source", outputs)

    def test_apple_toolchain_fallback_is_bounded_and_preserves_discovery_log(self):
        declared = inventory(metadata=True)
        declared.update(methods=[], method_count=0, symbol_count=1, supported_signature_count=0)
        candidate = str(self.root / "apple toolchain" / "swift-demangle")
        def which(name):
            return {"xcrun": "/usr/bin/xcrun", candidate: name}.get(name)
        def finder(argv, log, timeout):
            self.assertEqual(argv, ["/usr/bin/xcrun", "--find", "swift-demangle"])
            self.assertEqual(timeout, 10)
            log.write_text(candidate + "\n")
        with patch("mobile.swift_source.sys.platform", "darwin"), \
                patch("mobile.swift_source.shutil.which", side_effect=which), \
                patch("mobile.swift_source.run_tool", side_effect=finder), \
                patch("mobile.swift_source.recover_swift_signatures", return_value=declared) as recover:
            report, outputs = self.recover()
        self.assertEqual(recover.call_args.kwargs["demangler"], candidate)
        self.assertEqual(report["metadata_symbol_count"], 1)
        self.assertEqual(outputs["swift_toolchain_log"], "logs/swift-toolchain.log")

    def test_backend_report_rejects_false_counts_inventory_or_source(self):
        cases = []
        valid = source_batch()
        for key, value in (("schema_version", 2), ("status", "recovered"), ("source", "wrong source"),
                           ("method_count", True), ("recovered_method_count", 0),
                           ("unrecovered_method_count", 1), ("coverage_status", "partial"),
                           ("limitations", [None])):
            cases.append({**valid, key: value})
        wrong_identity = deepcopy(valid)
        wrong_identity["methods"][0]["entry"] = "0x2000"
        cases.append(wrong_identity)
        absent_source = deepcopy(valid)
        absent_source["methods"][0]["source"] = ""
        cases.append(absent_source)
        missing_reason = deepcopy(valid)
        missing_reason["methods"][0] = {"entry": "0x1000", "mangled_symbol": SYMBOL, "status": "unrecovered"}
        cases.append(missing_reason)
        for batch in cases:
            with self.subTest(batch=batch), self.assertRaises(MobileError):
                _coverage(inventory(), batch)

    def test_native_failure_and_absent_batch_are_errors(self):
        with patch("mobile.swift_source.shutil.which", return_value="demangler"), \
                patch("mobile.swift_source.recover_swift_signatures", return_value=inventory()):
            with patch("mobile.swift_source.run_tool", side_effect=MobileError("backend failed")), \
                    self.assertRaisesRegex(MobileError, "backend failed"):
                self.recover()
            with patch("mobile.swift_source.run_tool"), self.assertRaisesRegex(MobileError, "did not produce"):
                self.recover()

    def test_signature_byte_budget_applies_before_backend(self):
        with patch("mobile.swift_source.shutil.which", return_value="demangler"), \
                patch("mobile.swift_source.recover_swift_signatures", return_value=inventory()), \
                patch("mobile.swift_source.run_tool") as run, \
                self.assertRaisesRegex(MobileError, "signature inventory.*byte limit"):
            self.recover(limits=Limits(max_bytes=10))
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
