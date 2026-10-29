#!/usr/bin/env python3
"""Public CLI/C ABI parity over compiled, owned native code; no execution."""
import argparse
import ctypes
import json
import os
import pathlib
import queue
import signal
import subprocess
import tempfile
import threading
import time


def run(args, expected):
    result = subprocess.run(args, text=True, capture_output=True, timeout=60)
    assert result.returncode in expected, (args, result.returncode, result.stdout, result.stderr)
    return result


def stable(document):
    document.pop("timings", None)
    return document


def cancel_input_loading(args, root, small):
    if os.name != "posix":
        print("SKIP: CLI loading interrupt requires POSIX child SIGINT delivery")
        return
    large = root / "loading.bin"
    with large.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    project = pathlib.Path(str(large) + ".neverd")
    project.mkdir()
    (project / "load.json").write_text(json.dumps({
        "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": "0x1000"}))
    saved = root / "interrupted-review.nddiff"
    saved.write_text("existing review must survive\n")
    for side in ("primary", "secondary"):
        paths = (str(large), small) if side == "primary" else (small, str(large))
        output = root / f"stopped-{side}.json"
        with output.open("w") as stdout:
            process = subprocess.Popen([args.neverd, "diff", "--structural", "-a", paths[0],
                                        "-b", paths[1], "--json", "--save", str(saved)],
                                       stdout=stdout, stderr=subprocess.PIPE, text=True)
            messages, captured = queue.Queue(), []

            def drain():
                for line in process.stderr:
                    captured.append(line)
                    messages.put(line)
                messages.put(None)

            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            try:
                deadline = time.monotonic() + 15
                while True:
                    remaining = deadline - time.monotonic()
                    assert remaining > 0, captured
                    line = messages.get(timeout=remaining)
                    assert line is not None, captured
                    if line.startswith(f"diff: {side}:load:hash "):
                        process.send_signal(signal.SIGINT)
                        break
                process.wait(timeout=15)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=15)
                reader.join(timeout=5)
                process.stderr.close()
        assert process.returncode == 3, (side, process.returncode, captured)
        assert "input loading stopped; no comparison was published" in "".join(captured), captured
        assert output.read_text() == "", output.read_text()
        assert saved.read_text() == "existing review must survive\n"
    # An ordinary failed load remains an error, rather than a requested stop.
    run([args.neverd, "diff", "--structural", "-a", str(root / "missing.exe"), "-b", small], {1})


def cancel_saved_restore(args, root, base, saved):
    report = root / "恢复-取消.nddiff"
    document = json.loads(saved.read_text())
    document["warnings"].append("retained diagnostic " + "x" * (64 * 1024 * 1024))
    report.write_text(json.dumps(document))
    target = root / "已有-比较.nddiff"
    for stage in ("restore:read", "restore:parse", "restore:write"):
        target.write_text("existing review must survive\n")
        process = subprocess.Popen(base + ["--json", "--reopen", str(report), "--save", str(target)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        messages, captured = queue.Queue(), []

        def drain():
            for line in process.stderr:
                captured.append(line)
                messages.put(line)
            messages.put(None)

        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        try:
            deadline = time.monotonic() + 20
            while True:
                line = messages.get(timeout=max(0.1, deadline - time.monotonic()))
                assert line is not None and time.monotonic() < deadline, captured
                if line.startswith(f"diff: {stage} "):
                    process.send_signal(signal.SIGINT)
                    break
            process.wait(timeout=20)
            output = process.stdout.read()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)
            reader.join(timeout=5)
            process.stderr.close()
            process.stdout.close()
        assert process.returncode == 3, (stage, process.returncode, captured)
        assert "restore cancelled; no output was published" in "".join(captured), captured
        assert output == "", output
        assert target.read_text() == "existing review must survive\n"
    assert json.loads(run(base + ["--json", "--reopen", str(saved)], {2, 3}).stdout) == json.loads(saved.read_text())


def cancel_mapping_edits(args, root):
    if os.name != "posix":
        print("SKIP: CLI editing interrupt requires POSIX child SIGINT delivery")
        return
    paths = [root / "edit-primary.bin", root / "edit-secondary.bin"]
    add, xor = bytes.fromhex("83c001") * 3000, bytes.fromhex("83f003") * 3000
    for path, code in zip(paths, (add + xor, xor + add)):
        path.write_bytes(bytes.fromhex("b807000000") + code + b"\xc3")
        project = pathlib.Path(str(path) + ".neverd")
        project.mkdir()
        (project / "load.json").write_text(json.dumps({
            "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": "0x1000"}))
    base = [args.neverd, "diff", "--structural", "-a", str(paths[0]), "-b", str(paths[1])]
    decisions = root / "edit-decisions.json"
    decisions.write_text(json.dumps([{"kind": kind, "primary": "0x1000", "secondary": "0x1000"}
                                    for kind in ("unmatch", "match")]))
    report = root / "edit-before.nddiff"
    document = json.loads(run(base + ["--json", "--save", str(report)], {2, 3}).stdout)
    # A long saved diagnostic makes the actual output write interruptible.
    document["warnings"].append("retained diagnostic " + "x" * (64 * 1024 * 1024))
    report.write_text(json.dumps(document))
    target = root / "edit-retained.nddiff"
    for stage in ("edit:measure 0/1", "edit:write "):
        target.write_text("existing review must survive\n")
        output = root / "edit-stopped.json"
        command = base + ["--overrides", str(decisions), "--json", "--save", str(target)]
        if stage.startswith("edit:write"):
            command += ["--reopen", str(report)]
        with output.open("w") as stdout:
            process = subprocess.Popen(command, stdout=stdout, stderr=subprocess.PIPE, text=True)
            messages, captured = queue.Queue(), []

            def drain():
                for line in process.stderr:
                    captured.append(line)
                    messages.put(line)
                messages.put(None)

            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            try:
                deadline = time.monotonic() + 20
                while True:
                    line = messages.get(timeout=max(0.1, deadline - time.monotonic()))
                    assert line is not None and time.monotonic() < deadline, captured
                    if line.startswith("diff: " + stage):
                        process.send_signal(signal.SIGINT)
                        break
                process.wait(timeout=20)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=15)
                reader.join(timeout=5)
                process.stderr.close()
        assert process.returncode == 3, (stage, process.returncode, captured)
        assert "edit cancelled; no output was published" in "".join(captured), captured
        assert output.read_text() == "", output.read_text()
        assert target.read_text() == "existing review must survive\n"
    retried = json.loads(run(base + ["--overrides", str(decisions), "--json", "--save", str(target)], {2, 3}).stdout)
    assert len(retried["overrides"]) == 2 and retried["matches"][0]["confirmed"]
    assert json.loads(run(base + ["--reopen", str(target), "--json"], {2, 3}).stdout) == retried
    print("CLI edit cancellation: measurement/write stopped, old files retained, retry and reopen passed")


def ambiguity_pages(args, root):
    source = root / "copies.c"
    source.write_text("\n".join(f"int copy_{index:02d}(void) {{ return 7; }}" for index in range(40))
                      + "\nint comparison_marker = MARKER;\n")
    paths = [root / "copies-before.o", root / "copies-after.o"]
    for marker, path in enumerate(paths):
        run([args.compiler, "--target=x86_64-unknown-linux-gnu", "-c", "-O0",
             "-fno-asynchronous-unwind-tables", f"-DMARKER={marker}", str(source), "-o", str(path)], {0})
    base = [args.neverd, "diff", "--structural", "-a", str(paths[0]), "-b", str(paths[1]), "--ignore-names"]
    for side, opposite in (("secondary", "primary"), ("primary", "secondary")):
        inventory = json.loads(run(base + ["--list", "ambiguous", "--list-side", opposite,
                                          "--list-search", "copy_01"], {2, 3}).stdout)
        assert inventory["total"] == 1, inventory
        anchor = inventory["items"][0]["address"]
        query = ["--list", "all", "--list-side", side, "--ambiguous-with", anchor]
        first = json.loads(run(base + query + ["--limit", "7"], {2, 3}).stdout)
        assert first["total"] >= 39 and first["ambiguity"]["active"], first
        assert len(first["items"]) == 7 and first["next_offset"] == 7
        tail = json.loads(run(base + query + ["--offset", str(first["total"] - 3), "--limit", "7"], {2, 3}).stdout)
        assert len(tail["items"]) == 3 and tail["next_offset"] is None
        descending = json.loads(run(base + query + ["--list-sort", "address", "--list-descending", "--limit", "1"], {2, 3}).stdout)
        assert descending["items"][0] == tail["items"][-1]
        filtered = json.loads(run(base + query + ["--list-search", "copy_31"], {2, 3}).stdout)
        assert filtered["total"] == 1 and filtered["items"][0]["name"] == "copy_31"
        run(base + query + ["--offset", str(first["total"] + 1)], {1})
    for options in (["--ambiguous-with", "0x10"], ["--list", "reviews", "--list-side", "secondary"],
                    ["--list", "all", "--list-side", "wrong"], ["--list", "all", "--ambiguous-with", "0xdead"]):
        run(base + options, {1})
    run([args.neverd, "diff", "-a", str(paths[0]), "-b", str(paths[1]), "--list-search", "copy"], {1})


def mapped_call_regions(args, root):
    code = bytes.fromhex('89f883c00383f017e8130000006bc00783c00983f025c3')
    code += b'\x90' * (32 - len(code)) + bytes.fromhex('89f883c00bc3')
    paths = [root / 'call-regions-before.bin', root / 'call-regions-after.bin']
    for path, address in zip(paths, ('0x1000', '0x8000')):
        path.write_bytes(code)
        project = pathlib.Path(str(path) + '.neverd')
        project.mkdir()
        (project / 'load.json').write_text(json.dumps({
            'loader': 'binary', 'processor': 'x86_64', 'platform': 'sysv', 'base': address}))
    base = [args.neverd, 'diff', '--structural', '-a', str(paths[0]), '-b', str(paths[1])]
    report = root / 'call-regions.nddiff'
    legacy = json.loads(run(base + ['--regions', '--json'], {0, 2, 3}).stdout)
    assert legacy['regions']['anchor_count'] == 0, legacy['regions']
    result = json.loads(run(base + ['--region-calls', '--json', '--save', str(report)], {0, 2, 3}).stdout)
    assert result['regions']['profile'] == 'native-call-runs-v1'
    assert result['regions']['call_evidence'] == 'current_function_correspondence'
    assert result['regions']['anchor_count'] == 1, result['regions']
    assert result['regions']['semantic_validation'] == 'not_performed'
    assert json.loads(run(base + ['--reopen', str(report), '--json'], {0, 2, 3}).stdout) == result
    rows = json.loads(run(base + ['--reopen', str(report), '--region-anchor', '0'], {0, 2, 3}).stdout)
    assert rows['total'] == 7 and rows['rows'][3]['primary']['bytes'] == 'E813000000', rows
    overrides = root / 'call-regions-overrides.json'
    overrides.write_text(json.dumps([{'kind': 'unmatch', 'primary': '0x1020', 'secondary': '0x8020'}]))
    edited = json.loads(run(base + ['--reopen', str(report), '--overrides', str(overrides), '--json'], {2, 3}).stdout)
    assert edited['regions']['anchor_count'] == 0, edited['regions']


def unresolved_reference_evidence(args, root):
    paths = [root / "reference-before.bin", root / "reference-after.bin"]
    code = bytearray(24)
    code[:7] = bytes.fromhex("8b050a000000c3")
    code[16] = 17
    for index, path in enumerate(paths):
        code[23] = index
        path.write_bytes(code)
        project = pathlib.Path(str(path) + ".neverd")
        project.mkdir()
        (project / "load.json").write_text(json.dumps({
            "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": "0x1000"}))
    base = [args.neverd, "diff", "--structural", "-a", str(paths[0]), "-b", str(paths[1])]
    saved = root / "reference.nddiff"
    result = json.loads(run(base + ["--json", "--save", str(saved)], {3}).stdout)
    assert result["counts"]["changed"] == 0, result["counts"]
    assert result["counts"]["uncertain_without_observed_change"] == 1, result["counts"]
    rows = json.loads(run(base + ["--list", "uncertain", "--list-side", "primary"], {3}).stdout)
    assert rows["total"] == 1 and rows["items"][0]["evidence"]["change_state"] == "uncertain", rows
    assert json.loads(run(base + ["--list", "changed"], {3}).stdout)["total"] == 0
    assert json.loads(run(base + ["--reopen", str(saved), "--json"], {3}).stdout) == result
    assert "Uncertain: 1" in run(base, {3}).stdout


def mixed_reference_evidence(args, root):
    # The store destination is unresolved; its relocated immediate names a
    # string. Changing that known string must survive the other operand's gap.
    paths = []
    for index, data in enumerate(('.asciz "original-string"',
                                  '.asciz "original-string"',
                                  '.asciz "modified-string"', '.zero 16')):
        source, binary = root / f"mixed-{index}.s", root / f"mixed-{index}.o"
        source.write_text(".text\n.globl holder\n.type holder,@function\nholder:\n"
                          "movq $.Lmessage, 0(%rip)\nret\n.size holder,.-holder\n"
                          f".section .rodata\n.Lmessage:\n{data}\n.byte {index}\n")
        run([args.compiler, "--target=x86_64-unknown-linux-gnu", "-c", str(source),
             "-o", str(binary)], {0})
        paths.append(binary)
    for index in (1, 2, 3):
        for reverse in (False, True):
            left, right = (paths[index], paths[0]) if reverse else (paths[0], paths[index])
            base = [args.neverd, "diff", "--structural", "-a", str(left), "-b", str(right)]
            saved = root / f"mixed-{index}-{reverse}.nddiff"
            report = json.loads(run(base + ["--json", "--save", str(saved)], {3}).stdout)
            assert len(report["matches"]) == 1, report["matches"]
            evidence = report["matches"][0]
            assert not evidence["reference_comparison_complete"], evidence
            assert evidence["analysis_complete"], evidence
            assert ("references" in evidence["changes"]) == (index == 2), evidence
            assert ("operands" in evidence["changes"]) == (index == 2), evidence
            assert "bytes" not in evidence["changes"], evidence
            assert report["counts"]["changed"] == (index == 2), report["counts"]
            assert json.loads(run(base + ["--reopen", str(saved), "--json"], {3}).stdout) == report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    lib = ctypes.CDLL(args.library)
    pointer = ctypes.c_void_p
    lib.neverd_session_create.restype = pointer
    lib.neverd_session_load.argtypes = [pointer, ctypes.c_char_p]
    lib.neverd_session_destroy.argtypes = [pointer]
    lib.neverd_comparison_create.restype = pointer
    lib.neverd_comparison_run.argtypes = [pointer, pointer, pointer, ctypes.c_char_p, pointer, pointer]
    lib.neverd_comparison_export_json.argtypes = [pointer]
    lib.neverd_comparison_export_json.restype = pointer
    lib.neverd_comparison_impact_json.argtypes = [pointer, ctypes.c_char_p]
    lib.neverd_comparison_impact_json.restype = pointer
    lib.neverd_comparison_destroy.argtypes = [pointer]
    lib.neverd_free_string.argtypes = [pointer]
    with tempfile.TemporaryDirectory(prefix="neverd-diff-cli-") as folder:
        root = pathlib.Path(folder)
        ambiguity_pages(args, root)
        unresolved_reference_evidence(args, root)
        mapped_call_regions(args, root)
        mixed_reference_evidence(args, root)
        cancel_mapping_edits(args, root)
        for limit in (17, 9):
            run([args.compiler, "--target=x86_64-unknown-linux-gnu", "-c", "-O1", "-fno-asynchronous-unwind-tables",
                 f"-DPATCH_LIMIT={limit}", args.fixture, "-o", str(root / f"{limit}.o")], {0})
        a, b = str(root / "17.o"), str(root / "9.o")
        cancel_input_loading(args, root, a)
        base = [args.neverd, "diff", "--structural", "-a", a, "-b", b]
        saved = root / "review.nddiff"
        result = run(base + ["--json", "--save", str(saved)], {2, 3})
        doc = json.loads(result.stdout)
        assert doc["schema"] == "neverd.bindiff"
        assert doc["state"] == "complete"
        assert doc["matches"]
        assert any("constants" in row["changes"] for row in doc["matches"])
        assert json.loads(saved.read_text()) == doc
        assert "diff:" in result.stderr
        reopen = json.loads(run(base + ["--json", "--reopen", str(saved)], {2, 3}).stdout)
        assert reopen == doc
        contradictory = json.loads(json.dumps(doc))
        pair = contradictory["matches"][0]
        contradictory["overrides"].append({"kind": "unmatch", "primary": pair["primary"],
                                           "secondary": pair["secondary"]})
        invalid = root / "contradictory.nddiff"
        invalid.write_text(json.dumps(contradictory))
        retained = root / "retained.nddiff"
        retained.write_bytes(b"previous output\n")
        refused = run(base + ["--json", "--reopen", str(invalid), "--save", str(retained)], {1})
        assert not refused.stdout and "effective unmatch" in refused.stderr, refused
        assert retained.read_bytes() == b"previous output\n"
        cancel_saved_restore(args, root, base, saved)
        functions = json.loads(run(base + ["--list", "all", "--limit", "1"], {2, 3}).stdout)
        assert len(functions["items"]) == 1 and functions["next_offset"] == 1
        next_page = json.loads(run(base + ["--list", "all", "--offset", "1", "--limit", "1"], {2, 3}).stdout)
        assert next_page["items"][0]["address"] != functions["items"][0]["address"]
        detail = json.loads(run(base + ["--func", "checked", "--json"], {2, 3}).stdout)
        assert detail["primary"]["function"]["name"] == "checked"
        assert any(row["kind"] == "constant" for row in detail["rows"])
        checked = detail["primary"]["function"]["address"]
        source_args = ["--source-function", checked, "--source-representation", "c"]
        source_page = json.loads(run(base + source_args + ["--limit", "2"], {2, 3}).stdout)
        assert source_page["schema"] == "neverd.bindiff.source.page", source_page
        assert source_page["availability"] == "available", source_page
        assert source_page["input_sha256"] == doc["primary"]["sha256"]
        assert source_page["semantic_validation"] == "not_performed"
        assert len(source_page["document"]["rows"]) == 2
        source_next = json.loads(run(base + source_args + ["--limit", "2", "--offset", "2"], {2, 3}).stdout)
        assert source_next["document"]["offset"] == 2
        assert source_next["document"]["rows"][0]["line"] == 2
        limited_source = json.loads(run(base + source_args + ["--source-max-instructions", "1"], {3}).stdout)
        assert limited_source["state"] == "resource_limited" and not limited_source["document"]
        reopened_source = json.loads(run(base + source_args + ["--reopen", str(saved), "--limit", "2"], {2, 3}).stdout)
        assert reopened_source == source_page
        alignment_args = source_args + ["--source-difference", "--source-include-prelude"]
        alignment = json.loads(run(base + alignment_args, {2, 3}).stdout)
        assert alignment["schema"] == "neverd.bindiff.source.difference"
        assert alignment["availability"] == "available" and alignment["computation_complete"], alignment
        assert alignment["primary"]["input_sha256"] == doc["primary"]["sha256"]
        assert alignment["semantic_validation"] == "not_performed"
        first_alignment = json.loads(run(base + alignment_args + ["--limit", "2"], {2, 3}).stdout)
        assert first_alignment["rows"] == alignment["rows"][:2]
        next_alignment = json.loads(run(base + alignment_args + ["--offset", "2", "--limit", "2"], {2, 3}).stdout)
        assert next_alignment["rows"] == alignment["rows"][2:4]
        for option in ("--source-max-work", "--source-max-bytes"):
            stopped_alignment = json.loads(run(base + alignment_args + [option, "1"], {3}).stdout)
            assert stopped_alignment["state"] == "resource_limited" and not stopped_alignment["rows"]
            unlimited_alignment = json.loads(run(base + alignment_args + [option, "0"], {2, 3}).stdout)
            assert unlimited_alignment["rows"] == alignment["rows"], next(
                ((a, b) for a, b in zip(unlimited_alignment["rows"], alignment["rows"]) if a != b),
                (len(unlimited_alignment["rows"]), len(alignment["rows"])))
            run(base + source_args + [option, "1"], {1})
        run(base + ["--source-difference"], {1})
        run(base + ["--source-side", "secondary"], {1})
        legacy = [item for item in base if item != "--structural"]
        run(legacy + ["--source-function", checked], {1})
        run(legacy + ["--source-difference"], {1})
        run(base + source_args + ["--source-representation", "bad"], {1})
        assert json.loads(run(base + ["--func", checked, "--json"], {2, 3}).stdout) == detail
        assert "primary function not found" in run(base + ["--func", "check", "--json"], {1}).stderr
        changes_args = ["--changes-function", checked]
        native_changes = json.loads(run(base + changes_args, {2, 3}).stdout)
        assert native_changes["state"] == "complete" and native_changes["rows"]
        assert native_changes["primary_ssa"]["availability"] == "not_requested"
        changes = json.loads(run(base + changes_args + ["--dataflow"], {2, 3}).stdout)
        assert changes["rows"] and changes["primary_ssa"]["availability"] == "available"
        assert changes["fix_presence"] == "not_assessed" and changes["semantic_validation"] == "not_performed"
        assert any("constant" in row["kinds"] for row in changes["rows"])
        limited = json.loads(run(base + changes_args + ["--changes-max-work", "1"], {3}).stdout)
        assert limited["state"] == "resource_limited" and not limited["rows"]
        run(base + ["--changes-max-rows", "1"], {1})
        run(base + changes_args + ["--list", "all"], {1})
        run(base + changes_args + ["--changes-context", "authorization"], {1})
        journal_saved, record_path = root / "cases.nddiff", root / "record.json"
        record = {"primary": checked, "secondary": detail["secondary"]["function"]["address"],
                  "case": "owned-cli-case", "author": "CLI fixture analyst",
                  "note": "Manual fixture assessment, no engine proof.", "reference": "owned-test:cli",
                  "assessment": "candidate_fix", "expected_head": ""}
        record_path.write_text(json.dumps(record))
        history = json.loads(run(base + ["--review-record", str(record_path), "--save", str(journal_saved), "--list", "reviews"], {2, 3}).stdout)
        assert history["total"] == 1 and history["rows"][0]["source"] == "analyst"
        assert history["rows"][0]["semantic_validation"] == "not_performed"
        reopened = json.loads(run(base + ["--reopen", str(journal_saved), "--list", "reviews"], {2, 3}).stdout)
        assert reopened == history
        run(base + ["--review-record", str(record_path)], {1})
        run(base + ["--reopen", str(journal_saved), "--review-record", str(record_path), "--save", str(journal_saved)], {1})
        record["expected_head"], record["assessment"] = history["head"], "patch_present"
        record_path.write_text(json.dumps(record))
        latest = json.loads(run(base + ["--reopen", str(journal_saved), "--review-record", str(record_path),
                                       "--save", str(journal_saved), "--list", "reviews", "--review-latest"], {2, 3}).stdout)
        assert latest["record_count"] == 2 and latest["total"] == 1
        assert latest["rows"][0]["supersedes"] == history["head"]
        run(base + ["--review-case", "owned-cli-case"], {1})
        impact_args = ["--impact-function", checked]
        impact = json.loads(run(base + impact_args, {2, 3}).stdout)
        assert impact["state"] == "complete" and impact["function_count"] >= 2, impact
        assert any(row["name"] == "entry" and row["depth"] == 1 for row in impact["rows"]), impact
        assert impact["assessment"] == "possible_call_reachability"
        assert impact["parameter_return_binding"] == "not_modeled"
        call_sites = json.loads(run(base + impact_args + ["--impact-kind", "sites"], {2, 3}).stdout)
        assert call_sites["rows"] and all(row["instruction"]["bytes"] for row in call_sites["rows"])
        entry_row = next(row["row"] for row in impact["rows"] if row["name"] == "entry")
        path = json.loads(run(base + impact_args + ["--impact-kind", "path", "--impact-witness", str(entry_row)], {2, 3}).stdout)
        assert path["total"] == 1 and path["rows"][0]["caller"]["name"] == "entry"
        frontier = json.loads(run(base + impact_args + ["--impact-depth", "0", "--impact-kind", "frontier"], {3}).stdout)
        assert frontier["state"] == "resource_limited" and frontier["total"] > 0
        assert json.loads(run(base + impact_args + ["--reopen", str(saved)], {2, 3}).stdout) == impact
        run(base + ["--impact-depth", "0"], {1})
        run(base + impact_args + ["--impact-kind", "path"], {1})
        run(base + impact_args + ["--list", "all"], {1})
        same = json.loads(run([args.neverd, "diff", "--structural", "-a", a, "-b", a, "--json"], {0, 3}).stdout)
        assert all(not row["changes"] for row in same["matches"])
        legacy = json.loads(run([args.neverd, "diff", "-a", a, "-b", b, "--json"], {0}).stdout)
        assert "schema" not in legacy and "matched" in legacy
        run(base + ["--profile", "unsupported", "--json"], {1})
        options = root / "options.json"
        options.write_text(json.dumps({"max_instructions": 1}))
        limited = json.loads(run(base + ["--options", str(options), "--json"], {3}).stdout)
        assert limited["state"] == "resource_limited"
        # A saved result refuses the other direction; the old saved file survives.
        run([args.neverd, "diff", "--structural", "-a", b, "-b", a,
             "--reopen", str(saved), "--save", str(saved), "--json"], {1})
        assert json.loads(saved.read_text()) == doc
        # The exact same C API is the authority for CLI output.
        sa, sb, comparison = lib.neverd_session_create(), lib.neverd_session_create(), lib.neverd_comparison_create()
        try:
            assert lib.neverd_session_load(sa, a.encode()) == 1
            assert lib.neverd_session_load(sb, b.encode()) == 1
            assert lib.neverd_comparison_run(comparison, sa, sb, b"{}", None, None) == 1
            text = lib.neverd_comparison_export_json(comparison)
            try:
                api = json.loads(ctypes.string_at(text))
            finally:
                lib.neverd_free_string(text)
            assert stable(api) == stable(doc)
            text = lib.neverd_comparison_impact_json(comparison, json.dumps({"seeds": [checked]}).encode())
            assert text
            try:
                assert json.loads(ctypes.string_at(text)) == impact
            finally:
                lib.neverd_free_string(text)
        finally:
            lib.neverd_comparison_destroy(comparison)
            lib.neverd_session_destroy(sa)
            lib.neverd_session_destroy(sb)
        # Owned assembly gives exact split boundaries independent of compiler heuristics.
        region_fixture = pathlib.Path(args.fixture).with_name("region_fixture.S")
        for label, flags in (("whole", []), ("split", ["-DREGION_SPLIT=1"])):
            run([args.compiler, "--target=x86_64-unknown-linux-gnu", "-c", *flags, str(region_fixture), "-o", str(root / f"{label}.o")], {0})
        regions_base = [args.neverd, "diff", "--structural", "-a", str(root / "whole.o"), "-b", str(root / "split.o")]
        regions_saved = root / "regions.nddiff"
        regions = json.loads(run(regions_base + ["--regions", "--json", "--save", str(regions_saved)], {2, 3}).stdout)
        assert regions["regions"]["state"] == "complete"
        page = json.loads(run(regions_base + ["--list", "regions", "--region-kind", "split", "--limit", "1"], {2, 3}).stdout)
        assert page["total"] == 1, page
        candidate = page["items"][0]
        assert candidate["primary_function_count"] == 1 and candidate["secondary_function_count"] == 2
        anchors = json.loads(run(regions_base + ["--region", candidate["id"], "--limit", "1"], {2, 3}).stdout)
        assert anchors["total"] == 2 and anchors["next_offset"] == 1
        rows = json.loads(run(regions_base + ["--region", candidate["id"], "--region-anchor", str(anchors["items"][0]["anchor"]), "--limit", "2"], {2, 3}).stdout)
        assert rows["total"] == 5 and len(rows["rows"]) == 2
        assert json.loads(run(regions_base + ["--reopen", str(regions_saved), "--json"], {2, 3}).stdout) == regions
        run(regions_base + ["--region", "missing"], {1})
        options.write_text(json.dumps({"regions": {"max_anchors": 1}}))
        partial_saved = root / "regions-partial.nddiff"
        partial = json.loads(run(regions_base + ["--options", str(options), "--json", "--save", str(partial_saved)], {3}).stdout)
        assert partial["regions"]["anchor_count"] == 1 and not partial["regions"]["hypothesis_count"]
        ungrouped = json.loads(run(regions_base + ["--reopen", str(partial_saved), "--list", "region-anchors"], {3}).stdout)
        assert ungrouped["total"] == 1 and ungrouped["hypothesis"] is None
        original_rows = json.loads(run(regions_base + ["--reopen", str(partial_saved), "--region-anchor", "0", "--limit", "1"], {3}).stdout)
        assert original_rows["total"] == 5 and len(original_rows["rows"]) == 1
        flow_saved = root / "dependencies.nddiff"
        flow = json.loads(run(base + ["--dataflow", "--json", "--save", str(flow_saved)], {2, 3}).stdout)
        entries = flow["dataflow"]["primary"]["entries"]
        call_arguments = [node for row in entries if row["availability"] == "available"
                          for node in row["graph"]["nodes"]
                          if node["opcode"] in ("CALL", "INDIR_CALL") and node["call_argument_count"] > 0]
        assert call_arguments, [(row["function"], row["availability"], row.get("reason")) for row in entries]
        assert all(node["call_argument_begin"] >= 0 for node in call_arguments)
        available = next(row for row in entries if row["availability"] == "available")
        function = available["function"]
        node = next(index for index, row in enumerate(available["graph"]["nodes"]) if row["opcode"] == "INT_MULT")
        inventory = json.loads(run(base + ["--list", "dataflow", "--limit", "1"], {2, 3}).stdout)
        assert len(inventory["rows"]) == 1 and inventory["total"] == len(entries)
        nodes = json.loads(run(base + ["--flow-function", function, "--limit", "2"], {2, 3}).stdout)
        assert len(nodes["rows"]) == 2 and nodes["availability"] == "available"
        sliced = json.loads(run(base + ["--flow-function", function, "--slice-node", str(node), "--limit", "1"], {2, 3}).stdout)
        assert sliced["state"] == "complete" and len(sliced["rows"]) == 1
        assert sliced["semantic_validation"] == "not_performed"
        assert json.loads(run(base + ["--reopen", str(flow_saved), "--json"], {2, 3}).stdout) == flow
        run(base + ["--slice-node", "0"], {1})
        ssa_options = root / "ssa-region-options.json"
        ssa_options.write_text(json.dumps({"flow_regions": {"min_nodes": 2}}))
        ssa_saved = root / "ssa-regions.nddiff"
        ssa = json.loads(run(base + ["--flow-regions", "--options", str(ssa_options), "--json", "--save", str(ssa_saved)], {2, 3}).stdout)
        assert ssa["flow_regions"]["state"] == "complete", ssa["flow_regions"]
        ssa_base = base + ["--reopen", str(ssa_saved)]
        ssa_page = json.loads(run(ssa_base + ["--list", "flow-regions", "--limit", "1"], {2, 3}).stdout)
        assert ssa_page["total"] > 0 and len(ssa_page["items"]) == 1, ssa_page
        ssa_id = ssa_page["items"][0]["id"]
        ssa_anchors = json.loads(run(ssa_base + ["--flow-region", ssa_id, "--limit", "1"], {2, 3}).stdout)
        ssa_anchor = ssa_anchors["items"][0]["anchor"]
        ssa_nodes = json.loads(run(ssa_base + ["--flow-region-anchor", str(ssa_anchor), "--limit", "1"], {2, 3}).stdout)
        assert len(ssa_nodes["rows"]) == 1 and ssa_nodes["view"] == "nodes"
        assert ssa_nodes["analysis"]["semantic_validation"] == "not_performed"
        ssa_blocks = json.loads(run(ssa_base + ["--flow-region-anchor", str(ssa_anchor), "--flow-region-coordinates", "phi_blocks"], {2, 3}).stdout)
        assert ssa_blocks["view"] == "phi_blocks"
        assert json.loads(run(ssa_base + ["--json"], {2, 3}).stdout) == ssa
        run(base + ["--flow-region", "absent"], {1})
        run(base + ["--flow-region-coordinates", "phi_blocks"], {1})
        print("compiled ELF comparison, CLI/C API parity, paging, deltas, persistence, failure and budget checks passed")


if __name__ == "__main__":
    main()
