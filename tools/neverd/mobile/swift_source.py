"""Bounded Swift signature inventory and native source export workflow."""

from __future__ import annotations

from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import sys

from .common import Limits, MobileError, run_tool
from .swift_signatures import recover_swift_signatures


_LIMITATIONS = [
    "Swift source coverage counts classified callable symbols; stripped or unclassified symbols may conceal additional methods.",
    "Recovered Swift source is a projection of supported native bodies and signatures, not a semantic-equivalence certificate.",
    "Compiler-generated entries are explicitly labelled projections from recovered type source and native effect evidence, not independent ordinary source method bodies.",
]

_COMPILER_PROJECTIONS = {
    "allocating_initializer": "allocating_initializer",
    "destructor": "trivial_destructor",
    "deallocator": "deallocating_destructor",
    "type_metadata_accessor": "type_metadata_accessor",
    "modify_accessor": "modify_accessor",
    "modify_resume": "modify_resume",
}


def _write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def _unclassified_inventory(symbols: list[dict], reason: str) -> dict:
    return {"schema_version": 1, "methods": [], "symbols": [
        {"entry": symbol["address"], "mangled_symbol": symbol["name"],
         "classification": "unknown", "status": "unsupported", "reason": reason}
        for symbol in symbols], "method_count": 0, "symbol_count": len(symbols),
        "unclassified_symbol_count": len(symbols), "supported_signature_count": 0,
        "unsupported_signature_count": 0, "logs": [], "limitations": [reason] if reason else []}


def _coverage(inventory: dict, batch: dict | None, *, workflow_status: str | None = None) -> dict:
    """Bind backend coverage to the supplied inventory without guessing methods."""
    supplied = inventory["methods"]
    compiler_recovered = 0
    if batch is not None:
        if (not isinstance(batch, dict) or batch.get("schema_version") != 1 or
                batch.get("status") != "success" or not isinstance(batch.get("source"), str) or
                not isinstance(batch.get("methods"), list) or not isinstance(batch.get("limitations"), list) or
                not isinstance(batch.get("source_units"), list) or not isinstance(batch.get("types"), list) or
                not all(isinstance(item, dict) for item in batch["types"]) or
                not all(isinstance(item, str) for item in batch["limitations"])):
            raise MobileError("Swift backend source report has an unsupported schema")
        rows = batch["methods"]
        if any(not isinstance(row, dict) or not isinstance(row.get("entry"), str) or
               not isinstance(row.get("mangled_symbol"), str) for row in rows):
            raise MobileError("Swift backend report has an invalid method identity")
        identity = lambda row: (row["entry"], row["mangled_symbol"])
        if Counter(map(identity, rows)) != Counter(map(identity, supplied)):
            raise MobileError("Swift backend report disagrees with its method inventory")
        supplied_by_identity = {identity(row): row for row in supplied}
        compiler_rows = {}
        recovered = 0
        recovered_identities = []
        for row in rows:
            if row.get("status") == "recovered":
                if not isinstance(row.get("source"), str) or not row["source"].strip() or "\0" in row["source"]:
                    raise MobileError("Swift backend claimed recovery without method source")
                recovered += 1
                recovered_identities.append(identity(row))
                signature = supplied_by_identity[identity(row)]
                if signature.get("requires_runtime_source_proof") is True:
                    expected_kind = _COMPILER_PROJECTIONS.get(signature.get("runtime_source_kind"))
                    evidence = row.get("compiler_projection_evidence")
                    if (signature.get("declaration_kind") != "runtime" or not expected_kind or
                            row.get("source_representation") != "compiler-generated-from-type" or
                            row.get("compiler_projection_kind") != expected_kind or
                            not isinstance(evidence, list) or not 1 <= len(evidence) <= 128 or
                            not all(isinstance(item, str) and item.strip() and len(item) <= 8192 and "\0" not in item
                                    for item in evidence)):
                        raise MobileError("Swift compiler projection lacks its exact role or native evidence")
                    compiler_rows[identity(row)] = (row, signature)
                    compiler_recovered += 1
                elif (row.get("source_representation", "native-method-body") != "native-method-body" or
                      "compiler_projection_kind" in row or "compiler_projection_evidence" in row):
                    raise MobileError("Swift ordinary method cannot be relabelled as a compiler projection")
            elif row.get("status") != "unrecovered" or not isinstance(row.get("reason"), str) or not row["reason"]:
                raise MobileError("Swift backend omitted an unrecovered method reason")
        expected = {"method_count": len(rows), "recovered_method_count": recovered,
                    "unrecovered_method_count": len(rows) - recovered}
        if any(type(batch.get(key)) is not int or batch[key] != count for key, count in expected.items()):
            raise MobileError("Swift backend report disagrees with its method counts")
        representation_counts = {"source_body_method_count": recovered - compiler_recovered,
                                 "compiler_projection_method_count": compiler_recovered}
        if compiler_recovered or any(key in batch for key in representation_counts):
            if any(type(batch.get(key)) is not int or batch[key] != count
                   for key, count in representation_counts.items()):
                raise MobileError("Swift backend report disagrees with its source representation counts")
        callable_status = ("no-methods" if not rows else "unrecovered" if not recovered else
                           "recovered" if recovered == len(rows) else "partial")
        source_parts = []
        unit_identities = []
        units = []
        for unit in batch["source_units"]:
            if (not isinstance(unit, dict) or unit.get("kind") not in ("function", "type") or
                    not isinstance(unit.get("module"), str) or not unit["module"] or
                    not isinstance(unit.get("name"), str) or not unit["name"] or
                    not isinstance(unit.get("source"), str) or not unit["source"].strip() or "\0" in unit["source"] or
                    not isinstance(unit.get("method_entries"), list) or
                    not isinstance(unit.get("method_identities"), list) or
                    not all(isinstance(entry, str) and re.fullmatch(r"0x[0-9a-fA-F]+", entry)
                            for entry in unit["method_entries"]) or
                    not all(isinstance(item, dict) and isinstance(item.get("entry"), str) and
                            re.fullmatch(r"0x[0-9a-fA-F]+", item["entry"]) and
                            isinstance(item.get("mangled_symbol"), str) and item["mangled_symbol"]
                            for item in unit["method_identities"])):
                raise MobileError("Swift backend report has an invalid source unit")
            if unit["method_entries"] != [item["entry"] for item in unit["method_identities"]]:
                raise MobileError("Swift source units do not uniquely cover the recovered method identities: entry projection disagrees")
            for item in unit["method_identities"]:
                compiler = compiler_rows.get(identity(item))
                if compiler is not None:
                    row, signature = compiler
                    if (unit["kind"] != "type" or unit["module"] != signature.get("module") or
                            unit["name"] != signature.get("context_name") or row["source"] != unit["source"]):
                        raise MobileError("Swift compiler projection does not belong to its actual type source unit")
            source_parts.append(unit["source"] + "\n")
            unit_identities.extend(identity(item) for item in unit["method_identities"])
            units.append({key: unit[key] for key in ("kind", "module", "name", "method_entries", "method_identities")})
        if (len(set(recovered_identities)) != len(recovered_identities) or
                Counter(unit_identities) != Counter(recovered_identities)):
            raise MobileError("Swift source units do not uniquely cover the recovered methods")
        if batch.get("coverage_status") != callable_status or batch["source"] != "".join(source_parts):
            raise MobileError("Swift backend report disagrees with its emitted source or coverage")
        # Preserve the declaration alongside its result, but keep source text in
        # its actual .swift artifact rather than duplicating it in every report.
        by_identity: dict[tuple[str, str], list[dict]] = {}
        for row in rows:
            by_identity.setdefault(identity(row), []).append(row)
        methods = []
        for signature in supplied:
            row = by_identity[identity(signature)].pop(0)
            method = {key: value for key, value in signature.items() if key not in ("status", "reason")}
            method["signature_status"] = signature["status"]
            method["status"] = row["status"]
            if row["status"] == "unrecovered":
                method["reason"] = row["reason"]
            else:
                method["source_representation"] = row.get("source_representation", "native-method-body")
                if identity(row) in compiler_rows:
                    method["compiler_projection_kind"] = row["compiler_projection_kind"]
                    method["compiler_projection_evidence"] = row["compiler_projection_evidence"]
            methods.append(method)
        backend_limitations = batch["limitations"]
        types = batch["types"]
    else:
        if supplied:
            raise MobileError("Swift callable inventory has no native source report")
        methods = []
        recovered = 0
        callable_status = "no-methods"
        backend_limitations = []
        units = []
        types = []
    unknown = inventory["unclassified_symbol_count"]
    status = workflow_status or callable_status
    if unknown and workflow_status is None:
        status = "partial" if recovered else "unclassified"
    non_methods = []
    for symbol in inventory["symbols"]:
        row = dict(symbol)
        if row["classification"] == "metadata":
            row["status"] = "not-callable"
            row.pop("reason", None)
        else:
            row["status"] = "unclassified"
        non_methods.append(row)
    return {"schema_version": 1, "status": status, "coverage_status": callable_status,
            "method_count": len(methods), "recovered_method_count": recovered,
            "unrecovered_method_count": len(methods) - recovered,
            "source_body_method_count": recovered - compiler_recovered,
            "compiler_projection_method_count": compiler_recovered,
            "symbol_count": inventory["symbol_count"],
            "metadata_symbol_count": sum(row["classification"] == "metadata" for row in non_methods),
            "unclassified_symbol_count": unknown,
            "supported_signature_count": inventory["supported_signature_count"],
            "unsupported_signature_count": inventory["unsupported_signature_count"],
            "methods": methods, "non_method_symbols": non_methods,
            "source_units": units, "source_type_count": sum(unit["kind"] == "type" for unit in units),
            "type_metadata_count": len(types), "types": types,
            "limitations": list(dict.fromkeys([*_LIMITATIONS, *inventory["limitations"], *backend_limitations]))}


def recover_swift_sources(binary: Path, output: Path, *, symbols: list[dict], neverd: str,
                          demangler: str | None, pointer_size: int, max_func: int,
                          limits: Limits) -> tuple[dict, dict]:
    """Export source from native HighIR; no generated code calls the input binary."""
    if not isinstance(symbols, list) or len(symbols) > limits.max_files:
        raise MobileError("Swift symbol inventory exceeds the file limit")
    if any(not isinstance(symbol, dict) or not isinstance(symbol.get("name"), str) or
           not isinstance(symbol.get("address"), str) for symbol in symbols):
        raise MobileError("invalid Swift symbol inventory entry")
    inventory_path = output / "metadata/swift-signatures.json"
    outputs = {"swift_signatures": "metadata/swift-signatures.json",
               "swift_method_coverage": "metadata/swift-methods.json"}
    workflow_status = None
    if not symbols:
        inventory = _unclassified_inventory([], "")
        workflow_status = "no-symbols"
    elif pointer_size != 8:
        inventory = _unclassified_inventory(symbols, "Swift source projection requires a 64-bit Mach-O slice.")
        workflow_status = "unsupported-architecture"
    else:
        configured = demangler if demangler is not None else os.environ.get("NEVERD_SWIFT_DEMANGLE") or None
        if configured is not None and (not configured or "\0" in configured):
            raise MobileError("Swift demangler path is empty or invalid")
        tool = shutil.which(configured or "swift-demangle")
        if not tool and configured is not None:
            raise MobileError("configured Swift demangler is unavailable; check --swift-demangle or NEVERD_SWIFT_DEMANGLE")
        if not tool and configured is None and sys.platform == "darwin":
            finder = shutil.which("xcrun")
            if finder:
                finder_log = output / "logs/swift-toolchain.log"
                outputs["swift_toolchain_log"] = "logs/swift-toolchain.log"
                try:
                    run_tool([finder, "--find", "swift-demangle"], finder_log,
                             min(limits.timeout, 10))
                    candidate = finder_log.read_text(encoding="utf-8").strip()
                    if "\n" not in candidate and "\0" not in candidate and Path(candidate).is_absolute():
                        tool = shutil.which(candidate)
                except (MobileError, UnicodeError):
                    # Automatic toolchain lookup is optional. Its bounded log
                    # remains available, and unclassified coverage is explicit.
                    tool = None
        if not tool:
            inventory = _unclassified_inventory(symbols,
                "Swift demangler is unavailable; install swift-demangle or set --swift-demangle to classify signatures and recover source.")
            workflow_status = "unavailable"
        else:
            inventory = recover_swift_signatures(symbols, demangler=tool,
                log_directory=output / "logs", limits=limits, pointer_size=pointer_size)
            if inventory["logs"]:
                outputs["swift_demangle_logs"] = ["logs/" + name for name in inventory["logs"]]
    _write_json(inventory_path, inventory)
    if inventory_path.stat().st_size > min(limits.max_bytes, 32 * 1024 * 1024):
        raise MobileError("Swift signature inventory exceeds the byte limit")
    batch = None
    if inventory["methods"]:
        batch_path = output / "artifacts/swift-recovery.json"
        log = output / "logs/swift-native.log"
        command = [neverd, "export", str(binary), "--format=swift-methods",
                   "--source-signatures=" + str(inventory_path), "-o", str(batch_path)]
        if max_func:
            command.append(f"--max-func={max_func}")
        run_tool(command, log, limits.timeout)
        if not batch_path.is_file() or not batch_path.stat().st_size:
            raise MobileError("Swift backend did not produce a source report")
        if batch_path.stat().st_size > limits.max_bytes:
            raise MobileError("Swift backend source report exceeds the byte limit")
        try:
            batch = json.loads(batch_path.read_text(encoding="utf-8"))
        except (ValueError, UnicodeError, RecursionError) as error:
            raise MobileError("Swift backend produced an invalid source report") from error
        outputs["swift_native_log"] = "logs/swift-native.log"
    coverage = _coverage(inventory, batch, workflow_status=workflow_status)
    if batch is not None:
        if batch["source"]:
            (output / "sources/swift.swift").write_text(batch["source"], encoding="utf-8")
            outputs["swift_source"] = "sources/swift.swift"
        batch_path.unlink()
    _write_json(output / "metadata/swift-methods.json", coverage)
    return coverage, outputs
