#!/usr/bin/env python3
"""Shared structural comparison through the framed worker, never execute inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

from transport_test import Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def ambiguity_pages(client, root):
    # More than the inventory's preview, with independently known boundaries.
    body = bytes.fromhex("b80700000083c003c3") + b"\xcc" * 7
    inputs = []
    for side, base in (("primary", 0x1000), ("secondary", 0x8000)):
        path = root / f"candidates-{side}.bin"
        data = bytearray(body * 40)
        if side == "secondary":
            data[-1] = 0xdd
        path.write_bytes(data)
        inputs.append({"path": str(path), "load_options": {
            "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": hex(base)}})
    summary = ok(client, "comparison_start", {
        "primary": inputs[0], "secondary": inputs[1], "options": {"use_names": False}})
    revision = summary["comparison_revision"]
    for side, source in (("secondary", "0x1010"), ("primary", "0x8010")):
        query = {"side": side, "ambiguous_with": source, "limit": 7,
                 "expected_comparison_revision": revision}
        first = ok(client, "comparison_functions", query)
        assert first["total"] >= 39 and first["ambiguity"]["active"], first
        assert first["total"] == first["ambiguity"]["candidate_total"]
        addresses, page = [], first
        while True:
            addresses += [item["address"] for item in page["items"]]
            if page["next_offset"] is None:
                break
            page = ok(client, "comparison_functions", {**query, "offset": page["next_offset"]})
        assert len(set(addresses)) == len(addresses) == first["total"]
        assert addresses == sorted(addresses, key=lambda address: int(address, 16))
        filtered = ok(client, "comparison_functions", {**query, "search": addresses[-1]})
        assert filtered["total"] == 1 and filtered["items"][0]["address"] == addresses[-1]
        reversed_page = ok(client, "comparison_functions", {**query, "sort": "address", "descending": True})
        assert reversed_page["items"][0]["address"] == addresses[-1]
        refused = client.call("comparison_functions", {**query, "offset": len(addresses) + 1})
        assert refused["status"] == "error", refused
    query = {"side": "secondary", "ambiguous_with": "0x1010"}
    original = ok(client, "comparison_functions", query)
    edited = ok(client, "comparison_override", {
        "kind": "match", "primary": "0x1010", "secondary": "0x8030",
        "expected_comparison_revision": revision})
    stale = client.call("comparison_functions", {**query, "expected_comparison_revision": revision})
    assert stale["error"]["code"] == "stale_comparison", stale
    resolved = ok(client, "comparison_functions", query)
    assert resolved["total"] == 0 and not resolved["ambiguity"]["active"], resolved
    assert resolved["comparison_revision"] == edited["comparison_revision"]
    ok(client, "comparison_undo")
    restored = ok(client, "comparison_functions", query)
    assert restored["items"] == original["items"] and restored["ambiguity"] == original["ambiguity"]
    for source in ("0xdead", 4112):
        assert client.call("comparison_functions", {**query, "ambiguous_with": source})["status"] == "error"


def exhausted_ambiguities(client, root):
    """Every saved editing state must reopen, including the last candidate."""
    body = bytes.fromhex("b80700000083c003c3") + b"\xcc" * 7
    for counts in ((3, 4), (4, 3), (4, 4)):
        inputs = []
        for side, base, count in zip(("primary", "secondary"), (0x1000, 0x8000), counts):
            path = root / f"resolve-{counts[0]}-{counts[1]}-{side}.bin"
            path.write_bytes(body * count)
            inputs.append({"path": str(path), "load_options": {
                "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": hex(base)}})
        start = {"primary": inputs[0], "secondary": inputs[1], "options": {"use_names": False}}
        ok(client, "comparison_start", start)
        states = [ok(client, "comparison_export")]
        assert len(states[0]["ambiguities"]) == 1, states[0]
        group = states[0]["ambiguities"][0]
        pairs = list(zip(group["primary"], group["secondary"]))
        assert len(pairs) > 1, group
        for index, (primary, secondary) in enumerate(pairs):
            # Prime inventory caches before each mutation.
            for side in ("primary", "secondary"):
                ok(client, "comparison_functions", {"side": side, "kind": "ambiguous"})
            summary = ok(client, "comparison_override", {
                "kind": "match", "primary": primary, "secondary": secondary})
            assert summary["counts"]["ambiguities"] == int(index + 1 < len(pairs)), summary
            states.append(ok(client, "comparison_export"))
        for side in ("primary", "secondary"):
            remaining = len(group[side]) - len(pairs)
            page = ok(client, "comparison_functions", {"side": side, "kind": "ambiguous"})
            assert page["total"] == 0, page
            page = ok(client, "comparison_functions", {"side": side, "kind": "unmatched"})
            assert page["total"] == remaining, page
        for expected in reversed(states[:-1]):
            ok(client, "comparison_undo")
            assert ok(client, "comparison_export") == expected
        for expected in states[1:]:
            ok(client, "comparison_redo")
            assert ok(client, "comparison_export") == expected
        saved = root / f"resolved-{counts[0]}-{counts[1]}.nddiff"
        ok(client, "comparison_save", {"path": str(saved)})
        assert json.loads(saved.read_text()) == states[-1]
        ok(client, "comparison_start", {**start, "restore_path": str(saved)})
        assert ok(client, "comparison_export") == states[-1]
        # Reopen intermediate states as well, without consuming the undo
        # history used above. Saved bytes retain the analyst's exact choices.
        for expected in states[:-1]:
            saved.write_text(json.dumps(expected))
            restored = ok(client, "comparison_restore", {"path": str(saved)})
            assert restored["restore_completed"], restored
            assert ok(client, "comparison_export") == expected
    print("Resolved ambiguities: unequal sides, last pair, undo/redo and file reopen passed")


def cancellable_restore(client, root, start):
    """The restored result is transactional across read, bind and replacement."""
    original = ok(client, "comparison_start", start)
    rows = ok(client, "comparison_functions")["items"]
    pair = next(row for row in rows if row.get("counterpart"))
    address, counterpart = pair["address"], pair["counterpart"]
    if isinstance(counterpart, dict):
        counterpart = counterpart["address"]
    ok(client, "comparison_override", {"kind": "confirm", "primary": address,
                                        "secondary": counterpart})
    saved = root / "restore-cancel.nddiff"
    ok(client, "comparison_save", {"path": str(saved)})
    document = ok(client, "comparison_export")
    with saved.open("a") as out:
        out.write(" " * (64 * 1024 * 1024))
    # Keep a redo record as well as selected-row queries across both paths.
    ok(client, "comparison_undo")
    before = ok(client, "comparison_export")
    summary = ok(client, "comparison_summary")
    rows = ok(client, "comparison_functions")
    for operation, payload in (("comparison_restore", {"path": str(saved)}),
                               ("comparison_start", {**start, "restore_path": str(saved)})):
        request = client.send(operation, payload)
        client.next(lambda m: m.get("request_id") == request and m.get("status") == "progress"
                    and m.get("payload", {}).get("phase") == "restore:read", timeout=30)
        ack = ok(client, "cancel", {"request_id": request})
        assert ack["accepted"] and not ack["requires_restart"], ack
        stopped = client.response(request)
        assert stopped["status"] == "error" and stopped["error"]["code"] == "cancelled", stopped
        assert stopped["calculation_stopped"] and not stopped["completed_before_cancellation"], stopped
        assert not stopped["partial_result_retained"], stopped
        assert ok(client, "comparison_export") == before
        assert ok(client, "comparison_functions") == rows
        after = ok(client, "comparison_summary")
        assert after["comparison_revision"] == summary["comparison_revision"]
        assert after["can_redo"] and after["counts"] == summary["counts"]
    ok(client, "comparison_redo")
    assert ok(client, "comparison_export") == document
    # Full file restore also works beyond the backend reply limit. Starting a
    # new comparison reads settings locally before binding the same large report.
    for operation, payload in (("comparison_restore", {"path": str(saved)}),
                               ("comparison_start", {**start, "restore_path": str(saved)})):
        restored = ok(client, operation, payload)
        assert restored["restore_completed"], restored
        assert ok(client, "comparison_export") == document
    # Old result state describes the saved computation, not this new restore.
    for state in ("complete", "cancelled", "timed_out", "resource_limited"):
        historical = {**document, "state": state, "stopped_phase": "" if state == "complete" else "measure"}
        saved.write_text(json.dumps(historical))
        restored = ok(client, "comparison_restore", {"path": str(saved)})
        assert restored["restore_completed"] and restored["job"]["restore_state"] == "complete", restored
        assert restored["state"] == state
        replaced = ok(client, "comparison_start", {**start, "restore_path": str(saved)})
        assert replaced["restore_completed"] and replaced["state"] == state
    ok(client, "comparison_start", start)


def aarch64_profiles(client, args, root, fixture):
    """Qualified object-format cells, original evidence and CLI/worker parity."""
    for target, format_name in (("aarch64-unknown-linux-gnu", "elf"),
                                ("aarch64-pc-windows-msvc", "coff"),
                                ("arm64-apple-macos11", "macho")):
        paths = [root / f"aarch64-{format_name}-{limit}.o" for limit in (17, 9)]
        for limit, path in zip((17, 9), paths):
            subprocess.run([args.cc, f"--target={target}", "-c", "-O1",
                            "-fno-asynchronous-unwind-tables", f"-DPATCH_LIMIT={limit}",
                            str(fixture), "-o", str(path)], check=True, capture_output=True)
        start = {"primary": {"path": str(paths[0])}, "secondary": {"path": str(paths[1])},
                 "options": {"use_names": False, "dataflow": True,
                             "flow_regions": {"min_nodes": 2}}}
        summary = ok(client, "comparison_start", start)
        for side in ("primary", "secondary"):
            assert summary[side]["architecture"] == "aarch64", summary
            assert summary[side]["bits"] == 64 and summary[side]["format"] == format_name
            assert summary[side]["normalization"] == "native-aarch64-operands-v1"
        inventory = ok(client, "comparison_functions")
        address = next(row["address"] for row in inventory["items"]
                       if row["name"].lstrip("_") == "checked")
        detail = ok(client, "comparison_function", {"address": address})
        assert detail["computation_complete"], detail
        assert not detail["evidence"]["normalized_unchanged"], detail
        assert any(row["kind"] == "constant" for row in detail["rows"]), detail
        nodes = ok(client, "comparison_dataflow", {
            "address": address, "kind": "nodes", "limit": 200})
        assert nodes["availability"] == "available", nodes
        assert nodes["abi"] == summary["primary"]["abi"], nodes
        native_nodes = [row for row in nodes["rows"] if row["origin_sequence"] >= 0]
        original_addresses = {row["primary"]["address"].lower() for row in detail["rows"]
                              if row["primary"] is not None}
        assert native_nodes and all(row["address"].lower() in original_addresses
                                    for row in native_nodes), nodes
        sliced = ok(client, "comparison_slice", {
            "address": address, "seeds": [native_nodes[0]["id"]], "limit": 200})
        assert sliced["state"] == "complete" and sliced["rows"], sliced
        assert sliced["semantic_validation"] == "not_performed", sliced
        regions = ok(client, "comparison_flow_regions", {"limit": 1})
        assert regions["total"] > 0, regions
        anchors = ok(client, "comparison_flow_regions", {
            "region_id": regions["items"][0]["id"], "limit": 1})
        aligned = ok(client, "comparison_flow_regions", {
            "anchor": anchors["items"][0]["anchor"], "limit": 1})
        assert aligned["rows"] and aligned["view"] == "nodes", aligned
        assert aligned["analysis"]["semantic_validation"] == "not_performed", aligned
        source = ok(client, "comparison_source_difference", {"address": address})
        assert source["computation_complete"] and source["availability"] == "available", source
        assert any(row["kind"] == "changed" for row in source["rows"]), source
        document = ok(client, "comparison_export")
        saved = root / f"aarch64-{format_name}.nddiff"
        saved.write_text(json.dumps(document))
        cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(paths[0]),
                              "-b", str(paths[1]), "--reopen", str(saved), "--json"],
                             capture_output=True, text=True, timeout=60)
        assert cli.returncode in (2, 3), cli.stderr
        assert json.loads(cli.stdout) == document
        explicit = ok(client, "comparison_start", {
            **start, "options": {**start["options"], "profile": "native-aarch64-v1"},
            "restore_path": str(saved)})
        assert explicit["primary"] == summary["primary"]
        assert ok(client, "comparison_export") == document
        refused = client.call("comparison_start", {
            **start, "options": {"profile": "native-x86-v1"}})
        assert refused["status"] == "error", refused
        assert ok(client, "comparison_export") == document
    # Preserve the complete Mach-O subtype, including ARM64e variants/ABI bits.
    original = paths[0].read_bytes()
    for subtype in (2, 12, 0x80000002):
        variant = root / f"aarch64-subtype-{subtype:x}.o"
        modified = bytearray(original)
        modified[8:12] = subtype.to_bytes(4, "little")
        variant.write_bytes(modified)
        same = {"primary": {"path": str(variant)}, "secondary": {"path": str(variant)}}
        identified = ok(client, "comparison_start", same)
        assert identified["primary"]["abi"] != summary["primary"]["abi"], identified
        refused = client.call("comparison_start", {
            "primary": {"path": str(paths[0])}, "secondary": {"path": str(variant)}})
        assert refused["status"] == "error" and "ABI" in refused["error"]["message"], refused

    # An ELF class and machine tag are independent: do not admit AArch64/32.
    wrong_bits = root / "aarch64-wrong-bits.o"
    subprocess.run([args.cc, "--target=i386-unknown-linux-gnu", "-c", "-O1",
                    str(fixture), "-o", str(wrong_bits)], check=True, capture_output=True)
    modified = bytearray(wrong_bits.read_bytes())
    modified[18:20] = (183).to_bytes(2, "little")  # EM_AARCH64 in ELFCLASS32.
    wrong_bits.write_bytes(modified)
    invalid = {"path": str(wrong_bits)}
    refused = client.call("comparison_start", {"primary": invalid, "secondary": invalid})
    assert refused["status"] == "error", refused

    imported = root / "aarch64-import.c"
    imported.write_text("extern void *malloc(unsigned long);\n"
                        "unsigned long allocate(unsigned long n) {\n"
                        "  return (unsigned long)malloc(n) + 1;\n}\n")
    imported_object = root / "aarch64-import.o"
    subprocess.run([args.cc, "--target=aarch64-unknown-linux-gnu", "-c", "-O1",
                    "-fno-asynchronous-unwind-tables", str(imported), "-o", str(imported_object)],
                   check=True, capture_output=True)
    imported_input = {"path": str(imported_object)}
    ok(client, "comparison_start", {"primary": imported_input, "secondary": imported_input})
    rows = ok(client, "comparison_functions")["items"]
    allocate = next(row["address"] for row in rows if row["name"] == "allocate")
    effects = ok(client, "comparison_impact", {"seeds": [allocate], "kind": "effects"})
    assert effects["effect_review"]["qualified_contracts"] == 0, effects
    # An object's undefined symbol alone is not a qualified runtime import.
    assert any(row["effect"]["status"] == "unqualified_identity" for row in effects["rows"]), effects

    truncated = root / "aarch64-truncated.bin"
    truncated.write_bytes(bytes.fromhex("e00080d2000c0091c0035f"))
    raw = {"path": str(truncated), "load_options": {
        "loader": "binary", "processor": "aarch64", "base": "0x1000"}}
    partial = ok(client, "comparison_start", {"primary": raw, "secondary": raw,
                                               "options": {"discover": False}})
    assert partial["coverage"]["primary"]["incomplete"] > 0, partial
    assert partial["counts"]["normalized_unchanged"] == 0, partial
    print("AArch64 ELF/COFF/Mach-O comparison, source, SSA/slices/regions and saved CLI parity passed")



def optimized_ssa_profiles(client, args, root, fixture):
    source = fixture.with_name("compiler_regions_fixture.c")
    for fmt, target in (("elf", "aarch64-unknown-linux-gnu"),
                         ("coff", "aarch64-pc-windows-msvc"),
                         ("macho", "arm64-apple-macos11")):
        paths = [root / f"optimized-{fmt}-O{level}.o" for level in (0, 2)]
        for path, level in zip(paths, (0, 2)):
            subprocess.run([args.cc, f"--target={target}", "-c", f"-O{level}",
                            "-fno-asynchronous-unwind-tables", str(source), "-o", str(path)],
                           check=True, capture_output=True)
        start = {"primary": {"path": str(paths[0])}, "secondary": {"path": str(paths[1])},
                 "options": {"use_names": False, "dataflow": {"profile": "llvm-ssa-v1"},
                             "flow_regions": True}}
        summary = ok(client, "comparison_start", start)
        assert summary["flow_regions"]["anchor_count"] > 0, summary
        assert summary["flow_regions"]["profile"] == "llvm-neighborhoods-v1"
        entries = ok(client, "comparison_dataflow", {"kind": "functions"})["rows"]
        address = next(e["function"] for e in entries if e["availability"] == "available")
        nodes = ok(client, "comparison_dataflow", {"kind": "nodes", "address": address, "limit": 200})
        assert nodes["schema_version"] == 3 and nodes["profile"] == "llvm-ssa-v1", nodes
        native = next(n for n in nodes["rows"] if n["origins"])
        sliced = ok(client, "comparison_slice", {"address": address, "seeds": [native["id"]]})
        assert sliced["state"] == "complete" and sliced["rows"], sliced
        anchors = ok(client, "comparison_flow_regions", {"all_anchors": True, "limit": 1})
        paired = ok(client, "comparison_flow_regions", {"anchor": anchors["items"][0]["anchor"], "limit": 200})
        plural = False
        for row in paired["rows"]:
            for side in ("primary", "secondary"):
                node = row[side]
                assert node["original"] is None and node["origin_sequence"] == -1, node
                origins = {o["address"].lower() for o in node["origins"]}
                originals = {o["address"].lower() for o in node["originals"]}
                assert origins == originals, node
                assert all(o["bytes"] and o["mnemonic"] for o in node["originals"])
                plural |= len(origins) > 1
        assert plural, paired
        document = ok(client, "comparison_export")
        saved = root / f"optimized-{fmt}.nddiff"
        saved.write_text(json.dumps(document))
        cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(paths[0]),
                              "-b", str(paths[1]), "--reopen", str(saved), "--json"],
                             capture_output=True, text=True, timeout=60)
        assert cli.returncode in (0, 2), cli.stderr
        assert json.loads(cli.stdout) == document
        ok(client, "comparison_start", {**start, "restore_path": str(saved)})
        assert ok(client, "comparison_export") == document
        refused = client.call("comparison_start", {
            **start, "options": {"dataflow": {"profile": "unknown-ssa"}}})
        assert refused["status"] == "error", refused
        assert ok(client, "comparison_export") == document
    print("Optimized AArch64 O0/O2 SSA, plural origins, slices and saved CLI/worker parity passed")


def parameter_views(client, args, root, fixture):
    source = fixture.with_name("control_regions_fixture.c")
    for fmt, target in (("elf", "x86_64-unknown-linux-gnu"),
                        ("coff", "x86_64-pc-windows-msvc"),
                        ("macho", "x86_64-apple-darwin")):
        paths = [root / f"parameters-{fmt}-O{level}.o" for level in (0, 2)]
        for path, level in zip(paths, (0, 2)):
            subprocess.run([args.cc, f"--target={target}", "-c", f"-O{level}",
                            "-fno-asynchronous-unwind-tables", str(source), "-o", str(path)],
                           check=True, capture_output=True)
        start = {"primary": {"path": str(paths[0])}, "secondary": {"path": str(paths[1])},
                 "options": {"use_names": False, "dataflow": {"profile": "llvm-parameter-ssa-v1"},
                             "flow_regions": True}}
        summary = ok(client, "comparison_start", start)
        assert summary["flow_regions"]["anchor_count"] > 0, summary
        entries = ok(client, "comparison_dataflow", {"kind": "functions", "side": "secondary"})["rows"]
        address = next(e["function"] for e in entries if e["availability"] == "available")
        query = {"side": "secondary", "kind": "nodes", "address": address, "limit": 1}
        page = ok(client, "comparison_dataflow", query)
        assert page["schema_version"] == 4 and page["projected_parameter_count"] == 1
        assert "parameter_views" not in page
        projected = []
        for offset in range(page["total"]):
            row = ok(client, "comparison_dataflow", {**query, "offset": offset})["rows"][0]
            assert all(v["input"] < len(row["inputs"]) for v in row["parameter_views"])
            if any(v["source_bits"] == 64 and v["view_bits"] == 32 for v in row["parameter_views"]):
                projected.append(row)
        assert projected
        sliced = ok(client, "comparison_slice", {"side": "secondary", "address": address,
                    "seeds": [projected[0]["id"]], "limit": 1})
        assert sliced["schema_version"] == 4 and sliced["rows"]
        document = ok(client, "comparison_export")
        saved = root / f"parameters-{fmt}.nddiff"
        saved.write_text(json.dumps(document))
        cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(paths[0]),
                              "-b", str(paths[1]), "--reopen", str(saved), "--json"],
                             capture_output=True, text=True, timeout=60)
        assert cli.returncode in (0, 2), cli.stderr
        assert json.loads(cli.stdout) == document
        ok(client, "comparison_start", {**start, "restore_path": str(saved)})
        assert ok(client, "comparison_export") == document
    print("Native parameter views: three formats, bounded node pages, slices and CLI/worker restore passed")


def loop_progress(client, args, root, fixture):
    source = fixture.with_name("control_regions_fixture.c")
    for family, version in ((2, 1), (3, 2), (4, 3), (5, 4), (6, 4),
                            (5, 5), (6, 5), (7, 5), (8, 6), (9, 7)):
        source = fixture.with_name("invariant_bound_progress_fixture.c" if family == 9
                                   else "parameter_bound_progress_fixture.c" if family == 8
                                   else "modular_progress_fixture.c" if family == 7
                                   else "branch_progress_fixture.c" if family >= 5
                                   else "control_regions_fixture.c")
        for arch, target in (("x64", "x86_64-unknown-linux-gnu"),
                             ("aarch64", "aarch64-unknown-linux-gnu")):
            paths = [root / f"progress-f{family}-v{version}-{arch}-O{level}.o" for level in (0, 2)]
            for path, level in zip(paths, (0, 2)):
                subprocess.run([args.cc, f"--target={target}", "-c", f"-O{level}",
                                "-fno-asynchronous-unwind-tables", f"-DFAMILY={family}",
                                str(source), "-o", str(path)], check=True, capture_output=True)
            if family == 6:
                paths.reverse()
            paired = paths[0] if version >= 4 else paths[1]
            start = {"primary": {"path": str(paths[0])}, "secondary": {"path": str(paired)},
                     "options": {"use_names": False, "dataflow": {"profile": f"llvm-progress-ssa-v{version}"},
                                 "flow_regions": {"fragments": True}}}
            summary = ok(client, "comparison_start", start)
            assert summary["flow_regions"]["anchor_count"] > 0, summary
            entries = ok(client, "comparison_dataflow", {"kind": "functions", "side": "primary"})["rows"]
            address = next(e["function"] for e in entries if e["availability"] == "available")
            query = {"side": "primary", "kind": "nodes", "address": address, "limit": 1}
            page = ok(client, "comparison_dataflow", query)
            assert page["schema_version"] == 4 + version
            assert page["progress"]["kind"] == "counter_loops"
            assert "loops" not in page["progress"]
            witnesses = []
            for offset in range(page["total"]):
                row = ok(client, "comparison_dataflow", {**query, "offset": offset})["rows"][0]
                if row["progress_loop"]:
                    proof = row["progress_loop"]
                    assert all(proof[key] < page["total"] for key in ("counter", "update", "compare"))
                    if version >= 2:
                        assert proof["header"] < page["block_count"]
                    if version == 2:
                        assert proof["header"] != proof["block"]
                    witnesses.append(proof)
            assert witnesses
            if version >= 4:
                guard_count = 1 if family >= 7 else 2
                assert page["progress"]["loop_count"] == 1
                assert page["progress"]["backedge_count"] == 1
                assert page["progress"]["guard_count"] == guard_count
                assert len({proof["latch"] for proof in witnesses}) == 1
                assert len({proof["block"] for proof in witnesses}) == guard_count
                if version >= 5:
                    assert {proof["proof"] for proof in witnesses} == {
                        "invariant_modular" if family == 9 else
                        "parameter_modular" if family == 8 else
                        "modular" if family == 7 else "nowrap"}
                for proof in {w["block"]: w for w in witnesses}.values():
                    assert ok(client, "comparison_slice", {"side": "primary", "address": address,
                              "seeds": [proof["compare"]], "limit": 1})["rows"]
            if version == 3:
                assert page["progress"]["loop_count"] == 2
                assert len({proof["block"] for proof in witnesses}) == 2
            sliced = ok(client, "comparison_slice", {"side": "primary", "address": address,
                        "seeds": [witnesses[0]["update"]], "limit": 1})
            assert sliced["schema_version"] == 4 + version and sliced["rows"]
            document = ok(client, "comparison_export")
            saved = root / f"progress-f{family}-v{version}-{arch}.nddiff"
            saved.write_text(json.dumps(document))
            cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(paths[0]),
                                  "-b", str(paired), "--reopen", str(saved), "--json"],
                                 capture_output=True, text=True, timeout=60)
            assert cli.returncode in (0, 2), cli.stderr
            assert json.loads(cli.stdout) == document
            ok(client, "comparison_start", {**start, "restore_path": str(saved)})
            assert ok(client, "comparison_export") == document
            if version >= 5:
                forged = json.loads(json.dumps(document))
                witness = forged["dataflow"]["primary"]["entries"][0]["graph"]["progress"]["loops"][0]
                witness["proof"] = "nowrap" if witness["proof"] == "modular" else "modular"
                tampered = saved.with_suffix(".tampered.nddiff")
                tampered.write_text(json.dumps(forged))
                assert client.call("comparison_start", {**start, "restore_path": str(tampered)})["status"] == "error"
                ok(client, "comparison_start", {**start, "restore_path": str(saved)})
                assert ok(client, "comparison_export") == document
            if version == 4:
                # Only one optimization level retains the required premises.
                # Keep the unproved side visible without claiming a match.
                crossed = ok(client, "comparison_start", {**start, "secondary": {"path": str(paths[1])}})
                assert crossed["flow_regions"]["anchor_count"] == 0
                evidence = ok(client, "comparison_export")["dataflow"]
                assert evidence["primary"]["entries"][0]["graph"] == document["dataflow"]["primary"]["entries"][0]["graph"]
                unproven = evidence["secondary"]["entries"][0]["graph"]["progress"]
                assert unproven["kind"] == "unproven"
                assert unproven["source_contract"] == unproven["observed_contract"]
    print("Loop progress: x64/AArch64 proofs, paged witnesses, slices and CLI/worker restore passed")


def shared_fragments(client, args, root, fixture):
    source = fixture.with_name("control_regions_fixture.c")
    for fmt, target in (("elf", "aarch64-unknown-linux-gnu"),
                         ("coff", "aarch64-pc-windows-msvc"),
                         ("macho", "arm64-apple-macos11")):
        paths = [root / f"fragments-{fmt}-O{level}.o" for level in (0, 2)]
        for path, level in zip(paths, (0, 2)):
            subprocess.run([args.cc, f"--target={target}", "-c", f"-O{level}",
                            "-fno-asynchronous-unwind-tables", str(source), "-o", str(path)],
                           check=True, capture_output=True)
        start = {"primary": {"path": str(paths[0])}, "secondary": {"path": str(paths[1])},
                 "options": {"use_names": False, "dataflow": {"profile": "llvm-ssa-v1"},
                             "flow_regions": {"fragments": True}}}
        summary = ok(client, "comparison_start", start)
        assert summary["flow_regions"]["profile"] == "llvm-fragments-v1", summary
        anchors = ok(client, "comparison_flow_regions", {"all_anchors": True})
        anchor = next(a for a in anchors["items"] if a["frontier_count"] and a["node_count"] >= 4)
        query = {"anchor": anchor["anchor"], "coordinates": "frontier", "limit": 1}
        page = ok(client, "comparison_flow_regions", query)
        assert page["boundary_only"] and page["view"] == "frontier", page
        assert page["total"] == anchor["frontier_count"]
        boundary = []
        while True:
            boundary += page["rows"]
            if page["next_offset"] is None:
                break
            page = ok(client, "comparison_flow_regions", {**query, "offset": page["next_offset"]})
        matched = ok(client, "comparison_flow_regions", {"anchor": anchor["anchor"], "limit": 200})
        assert matched["total"] == anchor["node_count"]
        for side in ("primary", "secondary"):
            assert not ({row[side]["id"] for row in boundary} &
                        {row[side]["id"] for row in matched["rows"]})
            assert all(row[side]["opcode"] not in ("phi", "select") for row in matched["rows"])
            for row in boundary + matched["rows"]:
                node = row[side]
                assert {int(o["address"], 16) for o in node["origins"]} == {
                    int(o["address"], 16) for o in node["originals"]}, node
                assert all(o["bytes"] and o["mnemonic"] for o in node["originals"])
        assert any(row["primary"]["opcode"] == "phi" and row["secondary"]["opcode"] == "select"
                   for row in boundary), boundary
        document = ok(client, "comparison_export")
        saved = root / f"fragments-{fmt}.nddiff"
        saved.write_text(json.dumps(document))
        cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(paths[0]),
                              "-b", str(paths[1]), "--reopen", str(saved), "--json"],
                             capture_output=True, text=True, timeout=60)
        assert cli.returncode in (0, 2), cli.stderr
        assert json.loads(cli.stdout) == document
        ok(client, "comparison_start", {**start, "restore_path": str(saved)})
        assert ok(client, "comparison_export") == document
        assert ok(client, "comparison_flow_regions", {"anchor": anchor["anchor"], "limit": 200})["rows"] == matched["rows"]
        reopened = ok(client, "comparison_flow_regions", query)
        assert reopened["rows"] == boundary[:1]
    print("Shared AArch64 branch fragments: finite matched nodes, external PHI/select, CLI/worker reopen passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parents[3] / "unittests/diff/patch_fixture.c"
    with tempfile.TemporaryDirectory(prefix="neverd-comparison-worker-") as folder:
        root = Path(folder)
        for limit in (17, 9):
            subprocess.run([args.cc, "--target=x86_64-unknown-linux-gnu", "-c", "-O1", "-fno-asynchronous-unwind-tables",
                            f"-DPATCH_LIMIT={limit}", str(fixture), "-o", str(root / f"{limit}.o")],
                           check=True, capture_output=True)
        a, b = root / "17.o", root / "9.o"
        with Client(args.worker) as client:
            assert "comparison_start" in client.hello["capabilities"]
            assert {"comparison_start", "comparison_slice", "comparison_impact", "comparison_observations",
                    "comparison_function_page", "comparison_function",
                    "comparison_cfg_summary", "comparison_cfg_viewport", "comparison_cfg_focus", "comparison_restore"}.issubset(
                        client.hello["comparison_cancellable_operations"])
            assert client.call("comparison_functions")["error"]["code"] == "comparison_not_loaded"
            ambiguity_pages(client, root)
            exhausted_ambiguities(client, root)
            aarch64_profiles(client, args, root, fixture)
            optimized_ssa_profiles(client, args, root, fixture)
            parameter_views(client, args, root, fixture)
            loop_progress(client, args, root, fixture)
            shared_fragments(client, args, root, fixture)
            start = {"primary": {"path": str(a)}, "secondary": {"path": str(b)}, "options": {"regions": True, "dataflow": True, "flow_regions": {"min_nodes": 2}}}
            summary = ok(client, "comparison_start", start)
            assert summary["schema"] == "neverd.bindiff.summary"
            assert summary["counts"]["changed"] > 0 and "matches" not in summary
            assert not summary["stale"] and summary["job"]["computation_stopped"]
            assert client.call("metadata")["error"]["code"] == "not_loaded"
            flow = ok(client, "comparison_dataflow", {"limit": 10})
            function = next(row["function"] for row in flow["rows"] if row["availability"] == "available")
            nodes = ok(client, "comparison_dataflow", {"address": function, "kind": "nodes", "limit": 200})
            node = next(row["id"] for row in nodes["rows"] if row["opcode"] == "INT_MULT")
            sliced = ok(client, "comparison_slice", {"address": function, "seeds": [node], "limit": 1})
            assert sliced["state"] == "complete" and len(sliced["rows"]) == 1, sliced
            assert sliced["comparison_revision"] == nodes["comparison_revision"]
            assert sliced["semantic_validation"] == "not_performed"
            assert "comparison_flow_regions" in client.hello["capabilities"]
            ssa_regions = ok(client, "comparison_flow_regions", {"limit": 1})
            assert ssa_regions["total"] > 0, ssa_regions
            ssa_id = ssa_regions["items"][0]["id"]
            ssa_anchors = ok(client, "comparison_flow_regions", {"region_id": ssa_id, "limit": 1})
            ssa_nodes = ok(client, "comparison_flow_regions", {"anchor": ssa_anchors["items"][0]["anchor"], "limit": 1})
            assert len(ssa_nodes["rows"]) == 1 and ssa_nodes["view"] == "nodes"
            assert ssa_nodes["analysis"]["semantic_validation"] == "not_performed"
            assert ssa_nodes["comparison_revision"] == nodes["comparison_revision"]
            region_page = ok(client, "comparison_regions", {"limit": 1})
            assert region_page["total"] > 0, region_page
            hypothesis = region_page["items"][0]
            anchors = ok(client, "comparison_regions", {"region_id": hypothesis["id"], "limit": 1})
            shared = ok(client, "comparison_region_page", {"region_id": hypothesis["id"], "anchor": anchors["items"][0]["anchor"], "limit": 1})
            assert len(shared["rows"]) == 1 and shared["hypothesis"]["semantic_validation"] == "not_performed"
            page = ok(client, "comparison_functions", {"search": "checked", "limit": 1})
            assert page["total"] == 1, page
            entry = page["items"][0]
            address, other = entry["address"], entry["counterpart"]["address"]
            observations = ok(client, "comparison_observations", {"address": address})
            assert observations["profile"] == "native-ssa-changes-v1"
            assert observations["state"] == "complete" and observations["rows"], observations
            assert observations["fix_presence"] == "not_assessed"
            assert observations["comparison_revision"] == page["comparison_revision"]
            assert any("constant" in row["kinds"] for row in observations["rows"])
            limited_changes = ok(client, "comparison_observations", {"address": address, "max_work": 1})
            assert limited_changes["state"] == "resource_limited" and not limited_changes["rows"]
            assert "comparison_impact" in client.hello["capabilities"]
            impact_query = {"seeds": [address]}
            impact = ok(client, "comparison_impact", impact_query)
            assert impact["state"] == "complete" and impact["function_count"] >= 2, impact
            caller = next(row for row in impact["rows"] if row["name"] == "entry")
            assert caller["depth"] == 1 and caller["witness"]["instruction"]["bytes"]
            assert impact["assessment"] == "possible_call_reachability"
            assert impact["semantic_validation"] == "not_performed"
            assert impact["comparison_revision"] == page["comparison_revision"]
            witness = ok(client, "comparison_impact", {**impact_query, "kind": "path", "witness": caller["row"]})
            assert witness["total"] == 1 and witness["work"] == impact["work"]
            frontier = ok(client, "comparison_impact", {**impact_query, "max_depth": 0, "kind": "frontier"})
            assert frontier["state"] == "resource_limited" and frontier["total"] > 0
            detail = ok(client, "comparison_function", {"address": address})
            assert any(row["kind"] == "constant" for row in detail["rows"])
            first = ok(client, "comparison_function_page", {"address": address, "limit": 1})
            assert first["state"] == "complete" and first["computation_complete"]
            assert detail["state"] == "complete" and detail["computation_complete"]
            refreshed = ok(client, "comparison_function_page", {"address": address, "limit": 1, "refresh": True})
            assert refreshed == first
            assert first["total"] == len(detail["rows"]) and first["rows"] == detail["rows"][:1]
            assert "comparison_source_page" in client.hello["capabilities"]
            assert "comparison_source_page" in client.hello["comparison_cancellable_operations"]
            source_query = {"address": address, "representation": "c", "limit": 200,
                            "expected_comparison_revision": page["comparison_revision"]}
            source = ok(client, "comparison_source_page", source_query)
            assert source["availability"] == "available", source
            assert source["comparison_revision"] == page["comparison_revision"]
            assert source["input_source_binding"] == "owned_sessions"
            assert source["semantic_validation"] == "not_performed"
            assert not source["provenance_complete"] and not source["hard_deadline"]
            mapped = [anchor for row in source["document"]["rows"] for anchor in row["comparison_anchors"]]
            assert mapped and all(anchor["address"] == detail["rows"][anchor["listing_row"]]["primary"]["address"] for anchor in mapped)
            source_cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(root / "17.o"),
                "-b", str(root / "9.o"), "--source-function", address, "--source-representation", "c"],
                text=True, capture_output=True, timeout=60)
            assert source_cli.returncode in (2, 3), source_cli.stderr
            cli_source = json.loads(source_cli.stdout)
            assert cli_source["document"]["text"] == source["document"]["text"]
            limited_source = ok(client, "comparison_source_page", {**source_query, "max_native_instructions": 1})
            assert limited_source["state"] == "resource_limited" and not limited_source["document"]
            assert "comparison_source_difference" in client.hello["capabilities"]
            assert "comparison_source_difference" in client.hello["comparison_cancellable_operations"]
            aligned = ok(client, "comparison_source_difference", source_query)
            assert aligned["availability"] == "available" and aligned["computation_complete"], aligned
            assert aligned["comparison_revision"] == page["comparison_revision"]
            assert any(row["kind"] == "changed" for row in aligned["rows"]), aligned
            for row in aligned["rows"]:
                for side in ("primary", "secondary"):
                    if row[side]:
                        assert all(anchor["address"] in row[side]["addresses"]
                                   for anchor in row[side]["comparison_anchors"]), row
            aligned_page = ok(client, "comparison_source_difference", {**source_query, "limit": 2})
            assert aligned_page["cached"] and aligned_page["rows"] == aligned["rows"][:2]
            aligned_cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(root / "17.o"),
                "-b", str(root / "9.o"), "--source-function", address, "--source-difference"],
                text=True, capture_output=True, timeout=60)
            assert aligned_cli.returncode in (2, 3), aligned_cli.stderr
            # CLI has not requested native alignment for navigation yet.
            cli_aligned = json.loads(aligned_cli.stdout)
            def textual_rows(rows):
                return [(row["kind"], *(row[side]["text"] if row[side] else None
                         for side in ("primary", "secondary"))) for row in rows]
            assert textual_rows(cli_aligned["rows"]) == textual_rows(aligned["rows"])
            limited_alignment = ok(client, "comparison_source_difference", {**source_query, "max_work": 1})
            assert limited_alignment["state"] == "resource_limited" and not limited_alignment["rows"]
            graph = ok(client, "comparison_cfg_summary", {"address": address})
            bounds = graph["bounds"]
            view = ok(client, "comparison_cfg_viewport", {"address": address,
                      "layout_revision": graph["layout_revision"], **bounds, "scale": 1})
            assert view["nodes"] and all("comparison" in n for n in view["nodes"])
            block = view["nodes"][0]["address"]
            focus = ok(client, "comparison_cfg_focus", {"address": address, "block": block})
            assert focus["width"] > 0 and focus["height"] > 0
            revision = page["comparison_revision"]
            edited = ok(client, "comparison_override", {"kind": "confirm", "primary": address,
                        "secondary": other, "expected_comparison_revision": revision})
            assert edited["can_undo"] and not edited["can_redo"]
            stale_source = client.call("comparison_source_page", source_query)
            assert stale_source["error"]["code"] == "stale_comparison", stale_source
            stale_alignment = client.call("comparison_source_difference", source_query)
            assert stale_alignment["error"]["code"] == "stale_comparison", stale_alignment
            refused = client.call("comparison_override", {"kind": "unmatch", "primary": address,
                        "secondary": other, "expected_comparison_revision": revision})
            assert refused["error"]["code"] == "stale_comparison", refused
            assert ok(client, "comparison_undo")["can_redo"]
            assert ok(client, "comparison_redo")["can_undo"]
            reviews = ok(client, "comparison_reviews", {"address": address})
            assert reviews["head"] == "" and reviews["selection"]["pair_active"]
            record = {"primary": address, "secondary": other, "case": "owned-worker-case",
                      "author": "Worker fixture analyst", "note": "Manual test assessment without proof.",
                      "assessment": "candidate_fix", "reference": "owned-test:worker", "expected_head": reviews["head"]}
            edited = ok(client, "comparison_review_record", record)
            assert edited["counts"]["review_records"] == 1
            history = ok(client, "comparison_reviews", {"address": address})
            assert history["total"] == 1 and history["rows"][0]["source"] == "analyst"
            assert history["rows"][0]["semantic_validation"] == "not_performed"
            assert client.call("comparison_review_record", record)["status"] == "error"
            saved = root / "review.nddiff"
            ok(client, "comparison_save", {"path": str(saved)})
            document = ok(client, "comparison_export")
            assert json.loads(saved.read_text()) == document
            contradictory = json.loads(json.dumps(document))
            contradictory["overrides"].append({"kind": "unmatch", "primary": address, "secondary": other})
            invalid = root / "contradictory.nddiff"
            invalid.write_text(json.dumps(contradictory))
            before_failed_restore = ok(client, "comparison_summary")
            refused = client.call("comparison_restore", {"path": str(invalid)})
            assert refused["status"] == "error" and "effective unmatch" in str(refused), refused
            assert ok(client, "comparison_export") == document
            after_failed_restore = ok(client, "comparison_summary")
            assert after_failed_restore["job"]["restore_state"] == "failed"
            assert after_failed_restore["job"]["previous_result_retained"]
            assert {k: v for k, v in after_failed_restore.items() if k != "job"} == {
                k: v for k, v in before_failed_restore.items() if k != "job"}
            refused = client.call("comparison_start", {**start, "restore_path": str(invalid)})
            assert refused["status"] == "error" and "effective unmatch" in str(refused), refused
            assert ok(client, "comparison_export") == document
            ok(client, "comparison_override", {"kind": "unmatch", "primary": address, "secondary": other})
            inactive = ok(client, "comparison_reviews", {"address": address})
            assert not inactive["rows"][0]["pair_active"] and inactive["head"] == history["head"]
            assert ok(client, "comparison_restore", {"path": str(saved)})["counts"] == edited["counts"]
            ok(client, "comparison_start", {**start, "restore_path": str(saved)})
            assert ok(client, "comparison_export") == document
            reopened_reviews = ok(client, "comparison_reviews", {"address": address})
            assert reopened_reviews["rows"] == history["rows"] and reopened_reviews["head"] == history["head"]
            reopened_impact = ok(client, "comparison_impact", impact_query)
            assert reopened_impact["rows"] == impact["rows"]
            # Failure does not replace the previous result or touch the owner's project.
            failed = client.call("comparison_start", {**start, "options": {"profile": "unsupported"}})
            assert failed["error"]["code"] == "comparison_failed", failed
            assert ok(client, "comparison_export") == document
            # CLI reopens the identical shared file including manual evidence and times.
            cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(a), "-b", str(b),
                                  "--reopen", str(saved), "--json"], capture_output=True, text=True, timeout=30)
            assert cli.returncode in (2, 3), cli.stderr
            assert json.loads(cli.stdout) == document
            # A changed file invalidates browsing and mapping but can still export its snapshot.
            a.write_bytes(a.read_bytes() + b"changed")
            assert ok(client, "comparison_summary")["stale"]
            assert client.call("comparison_functions")["error"]["code"] == "stale_input"
            assert client.call("comparison_impact", impact_query)["error"]["code"] == "stale_input"
            assert ok(client, "comparison_export") == document
            # A stopped detail remains stopped across pages and graph requests;
            # only explicit refresh supplies new comparison annotations.
            detail_path = root / "large-detail.bin"
            detail_path.write_bytes(b"\x90" * 60000 + b"\xc3")
            detail_input = {"path": str(detail_path), "load_options": {
                "loader": "binary", "processor": "x86_64", "base": "0x1000", "platform": "sysv"}}
            ok(client, "comparison_start", {"primary": detail_input, "secondary": detail_input,
                                             "options": {"discover": False}})
            detail_query = {"address": "0x1000", "limit": 1, "timeout_ms": 1}
            stopped = ok(client, "comparison_function_page", {**detail_query, "refresh": True})
            assert stopped["state"] == "timed_out" and not stopped["computation_complete"], stopped
            assert ok(client, "comparison_function_page", detail_query) == stopped
            stopped_graph = client.call("comparison_cfg_summary", {"address": "0x1000", "timeout_ms": 1})
            assert stopped_graph["error"]["code"] == "timed_out", stopped_graph
            fresh = ok(client, "comparison_function_page", {"address": "0x1000", "limit": 1, "refresh": True})
            assert fresh["state"] == "complete" and fresh["total"] == 60001
            assert ok(client, "comparison_cfg_summary", {"address": "0x1000"})["analysis_complete"]
            expired_changes = ok(client, "comparison_observations", {**detail_query, "refresh": True})
            assert expired_changes["state"] == "timed_out", expired_changes
            assert ok(client, "comparison_observations", detail_query) == expired_changes
            fresh_changes = ok(client, "comparison_observations", {"address": "0x1000", "refresh": True})
            assert fresh_changes["state"] == "complete" and fresh_changes["visited_native_rows"] == 60001
            # A replacement stopped during either input load never replaces
            # the existing comparison, its mapping revision or its query state.
            loading = root / "loading.bin"
            with loading.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            loading_input = {"path": str(loading), "load_options": {
                "loader": "binary", "processor": "x86_64", "base": "0x1000",
                "platform": "sysv"}}
            retained_summary = ok(client, "comparison_summary")
            retained_rows = ok(client, "comparison_functions")
            for side in ("primary", "secondary"):
                replacement = {"primary": start["primary"], "secondary": start["secondary"]}
                replacement[side] = loading_input
                request = client.send("comparison_start", replacement)
                progress = client.next(
                    lambda m: m.get("request_id") == request and m.get("status") == "progress"
                    and m.get("payload", {}).get("phase") == f"{side}:load:hash"
                    and m["payload"].get("done", 0) > 0, timeout=30)
                assert progress["payload"]["done"] < progress["payload"]["total"], progress
                load_stop_began = time.monotonic()
                acknowledged = ok(client, "cancel", {"request_id": request})
                assert acknowledged["accepted"] and not acknowledged["requires_restart"], acknowledged
                stopped_load = client.response(request)
                assert stopped_load["status"] == "error" and stopped_load["error"]["code"] == "cancelled", stopped_load
                assert stopped_load["calculation_stopped"] and not stopped_load["completed_before_cancellation"], stopped_load
                assert not stopped_load["partial_result_retained"], stopped_load
                assert ok(client, "comparison_summary") == retained_summary
                assert ok(client, "comparison_functions") == retained_rows
                print(f"{side} input loading stop and retained comparison: {time.monotonic() - load_stop_began:.3f}s")
            cancellable_restore(client, root, start)
            retried = ok(client, "comparison_start", start)
            assert retried["state"] == "complete", retried
            # Actual cooperative cancellation: acknowledge request while work is running;
            # publish 'stopped' and retained partial state only in the terminal response.
            large = root / "large.bin"
            large.write_bytes(b"\x90" * (8 * 1024 * 1024) + b"\xc3")
            raw = {"path": str(large), "load_options": {"loader": "binary", "processor": "x86_64", "base": "0x1000"}}
            request = client.send("comparison_start", {"primary": raw, "secondary": raw})
            client.next(lambda m: m.get("request_id") == request and m.get("status") == "progress"
                        and m.get("payload", {}).get("phase") == "primary:discovery", timeout=30)
            began = time.monotonic()
            cancel = ok(client, "cancel", {"request_id": request})
            assert cancel["accepted"] and not cancel["stopped"] and not cancel["requires_restart"], cancel
            terminal = client.response(request)
            elapsed = time.monotonic() - began
            assert terminal["status"] == "ok", terminal
            assert terminal["payload"]["state"] == "cancelled", terminal
            assert terminal["payload"]["stopped_phase"] == "discovery", terminal
            assert not terminal["payload"]["primary"]["discovery_complete"], terminal
            assert not terminal["payload"]["secondary"]["discovery_complete"], terminal
            assert terminal["partial_result_retained"] and not terminal["completed_before_cancellation"]
            assert terminal["payload"]["job"]["computation_stopped"]
            assert elapsed < 5, elapsed
            print(f"worker comparison parity, edits, stale/failure isolation and cancellation passed ({elapsed:.3f}s)")


if __name__ == "__main__":
    main()
