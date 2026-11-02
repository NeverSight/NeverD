#!/usr/bin/env python3
"""Generate owned native matching diagnostics and evaluate retained tool results.

These small fixtures are development diagnostics, not held-out quality or
competitive performance qualification. Linker-map truth is never a tool input.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
import time

if __package__:
    from . import benchmark_binary_diff_binexport as binexport
else:
    import benchmark_binary_diff_binexport as binexport

CORPUS_SCHEMA = "neverd.benchmark.diff.corpus.v1"
PARTIAL_CORPUS_SCHEMA = "neverd.benchmark.diff.corpus.v2"
RUN_SCHEMA = "neverd.benchmark.diff.run.v1"
RESULT_SCHEMA = "neverd.benchmark.diff.result.v1"
DEFAULT_TIMEOUT_SECONDS = 180.0
HASH_BLOCK_BYTES = 1024 * 1024
ADDRESS_BITS = 64
EVALUATOR_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path, algorithm: str = "sha256") -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as source:
        for block in iter(lambda: source.read(HASH_BLOCK_BYTES), b""):
            value.update(block)
    return value.hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, value: dict) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def recorded_path(path: Path, root: Path) -> str:
    try:
        return os.path.relpath(path, root)
    except ValueError:
        # Windows cannot express a relative path across drive letters. The
        # receipt reader also accepts an absolute path and still checks its hash.
        return str(path.resolve())


def artifact(path: Path, root: Path) -> dict:
    return {"path": recorded_path(path, root), "sha256": digest(path)}


def checked_artifact(record: dict, root: Path) -> Path:
    path = (root / record["path"]).resolve()
    require(digest(path) == record["sha256"], f"SHA-256 mismatch: {path}")
    return path


def address(value: object, base: int = 0) -> int:
    require(type(value) is int or isinstance(value, str), "address must be an integer or string")
    number = value if type(value) is int else int(value, base)
    require(0 <= number < 1 << ADDRESS_BITS, "address is outside uint64")
    return number


def truth_pairs(rows: list[dict]) -> set[tuple[int, int]]:
    """Admit complete one-to-one truth, including added and removed functions."""
    require(bool(rows), "ground truth must not be empty")
    ids, primary, secondary, pairs = set(), set(), set(), set()
    for row in rows:
        identity = row["id"]
        require(isinstance(identity, str) and identity and identity not in ids,
                "ground-truth identities must be unique and nonempty")
        ids.add(identity)
        left = None if row["primary"] is None else address(row["primary"])
        right = None if row["secondary"] is None else address(row["secondary"])
        require(left is not None or right is not None, "ground-truth row has no function")
        for value, seen in ((left, primary), (right, secondary)):
            if value is not None:
                require(value not in seen, "ground-truth address is assigned more than once")
                seen.add(value)
        if left is not None and right is not None:
            pairs.add((left, right))
    return pairs


def corpus_cases(corpus: dict) -> list[dict]:
    require(corpus["schema"] in (CORPUS_SCHEMA, PARTIAL_CORPUS_SCHEMA),
            "unsupported corpus schema")
    cases = corpus["cases"]
    require(isinstance(cases, list) and bool(cases), "corpus must contain cases")
    identities = set()
    for case in cases:
        identity = case["id"]
        require(isinstance(identity, str) and
                re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", identity) is not None,
                "case identity must be a single portable directory name")
        require(identity not in identities, "case identities must be unique")
        identities.add(identity)
        if corpus["schema"] == CORPUS_SCHEMA:
            require(case["truth_complete"] is True,
                    "this evaluator requires complete function truth")
        else:
            require(case["truth_complete"] is False,
                    "v2 requires explicitly partial function truth")
            require(case["truth_kind"] == "unique-source-bodies-v1",
                    "unsupported partial truth contract")
            require(isinstance(case["truth_artifacts"], list) and
                    bool(case["truth_artifacts"]), "partial truth requires evidence")
            for side in ("primary", "secondary"):
                address(case[side]["image_base"])
            require(all(row["primary"] is not None and row["secondary"] is not None
                        for row in case["truth"]),
                    "partial source truth cannot infer added or removed functions")
        truth_pairs(case["truth"])
    return cases


def score(expected: set[tuple[int, int]], observed: list[tuple[int, int]]) -> dict:
    actual = set(observed)
    correct, wrong, missed = actual & expected, actual - expected, expected - actual
    encode = lambda pairs: [[hex(a), hex(b)] for a, b in sorted(pairs)]
    return {
        "expected_pairs": len(expected), "accepted_pairs": len(actual),
        "duplicate_evidence_rows": len(observed) - len(actual),
        "correct": len(correct), "wrong": len(wrong), "missed": len(missed),
        "precision": len(correct) / len(actual) if actual else None,
        "recall": len(correct) / len(expected) if expected else None,
        "f1": 2 * len(correct) / (len(actual) + len(expected))
        if actual or expected else None,
        "wrong_pairs": encode(wrong), "missed_pairs": encode(missed),
        "exact_mapping": not wrong and not missed,
    }


def score_partial(expected: set[tuple[int, int]], observed: list[tuple[int, int]]) -> dict:
    """Judge known endpoints only; neither unlabelled endpoint implies an error."""
    left = {a for a, _ in expected}
    right = {b for _, b in expected}
    actual = set(observed)
    scored = {(a, b) for a, b in actual if a in left or b in right}
    unscored = actual - scored
    result = score(expected, list(scored))
    result.update({
        "truth_complete": False, "accepted_pairs": len(actual),
        "scored_accepted_pairs": len(scored), "unscored": len(unscored),
        "unscored_pairs": [[hex(a), hex(b)] for a, b in sorted(unscored)],
        "scored_accepted_fraction": len(scored) / len(actual) if actual else None,
        "duplicate_evidence_rows": len(observed) - len(actual),
        "scored_mapping_exact": result["exact_mapping"], "exact_mapping": None,
        "precision_scope": "accepted pairs with at least one labelled endpoint",
    })
    return result


def mapping_passed(result: dict) -> bool:
    metrics = result.get("metrics", {})
    return metrics.get("scored_mapping_exact", metrics.get("exact_mapping")) is True


def metric_summary(result: dict) -> dict:
    return {key: value for key, value in result.get("metrics", result).items()
            if not isinstance(value, (list, dict))}


def checked_diaphora_export(receipt: Path, database: Path, source: Path) -> dict:
    value = load(receipt)
    require(value["schema"] == "neverd.benchmark.diaphora-export.v1" and
            value["complete"] is True, "Diaphora export did not complete")
    require(value["input_sha256"] == digest(source), "IDA analyzed a different input")
    require(bool(value["ida_version"]) and bool(value["diaphora_version"]),
            "Diaphora/IDA version is missing")
    require(value["export_sha256"] == digest(database), "Diaphora export bytes changed")
    with closing(sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
        identities = connection.execute("SELECT md5sum FROM program").fetchall()
        require(len(identities) == 1, "Diaphora export must contain one program")
        identity = identities[0][0]
        # IDA's native MD5 is a 16-byte BLOB in current Diaphora exports;
        # older exports can store its hexadecimal spelling instead.
        identity = identity.hex() if isinstance(identity, bytes) else identity
        require(isinstance(identity, str) and identity.lower() == digest(source, "md5"),
                "Diaphora native program identity differs")
        require(connection.execute("SELECT value FROM version").fetchall() == [(value["diaphora_version"],)],
                "Diaphora native export version differs")
    return value


def read_diaphora_native(path: Path, inputs: list[Path]) -> list[tuple[int, int]]:
    receipt = load(path)
    require(receipt["schema"] == "neverd.benchmark.diaphora-match.v1" and
            receipt["complete"] is True, "Diaphora matching did not complete")
    require(receipt.get("worker_errors", []) == [], "Diaphora heuristic worker failed")
    require(receipt["input_sha256"] == [digest(source) for source in inputs],
            "Diaphora matching input direction differs")
    exports = []
    records = [receipt["result"]]
    for side, source in zip(("primary", "secondary"), inputs):
        record = receipt[side]
        records.extend((record["database"], record["export_receipt"]))
        database = checked_artifact(record["database"], path.parent)
        declaration = checked_artifact(record["export_receipt"], path.parent)
        exports.append(checked_diaphora_export(declaration, database, source))
    require(all(value["diaphora_version"] == receipt["diaphora_version"] for value in exports),
            "Diaphora matching and export versions differ")
    result = checked_artifact(receipt["result"], path.parent)
    with closing(sqlite3.connect(result.resolve().as_uri() + "?mode=ro", uri=True)) as database:
        require(database.execute("SELECT main_db, diff_db, version FROM config").fetchall() ==
                [(exports[0]["database_path"], exports[1]["database_path"], receipt["diaphora_version"])],
                "Diaphora native comparison direction or version differs")
    pairs = read_pairs("diaphora", result, inputs)
    for record in records:
        checked_artifact(record, path.parent)
    return pairs


def read_pairs(kind: str, path: Path, inputs: list[Path]) -> list[tuple[int, int]]:
    """Read selected function associations, never candidate or data matches."""
    if kind == "diaphora-native":
        return read_diaphora_native(path, inputs)
    if kind == "neverd":
        report = load(path)
        require(report["schema"] == "neverd.bindiff" and report["schema_version"] in (3, 4),
                "unsupported NeverD report schema")
        require(report["state"] == "complete", "NeverD comparison did not complete")
        for side, source in zip(("primary", "secondary"), inputs):
            require(report[side]["sha256"] == digest(source),
                    f"NeverD {side} input identity does not match the corpus")
        return [(address(row["primary"]), address(row["secondary"]))
                for row in report["matches"]]
    if kind in ("ghidra", "ghidra-vt"):
        report = load(path)
        schema = "neverd.benchmark.ghidra-vt.v1" if kind == "ghidra-vt" else "neverd.baseline.ghidra-vt-smoke.v1"
        require(report["schema"] == schema,
                "unsupported Ghidra export schema")
        if kind == "ghidra-vt":
            require(report["complete"] is True, "Ghidra export did not complete")
            for key, source in zip(("source_sha256", "destination_sha256"), inputs):
                require(report[key] == digest(source), "Ghidra input identity does not match the corpus")
        rows = [row for group in report["match_sets"] for row in group["matches"]
                if row["type"] == "Function" and row["status"] == "ACCEPTED"]
        require(all((row["source_space"], row["destination_space"]) in
                    (("ram", "ram"), ("EXTERNAL", "EXTERNAL")) for row in rows),
                "Ghidra address-space mapping needs an explicit adapter")
        # EXTERNAL addresses name Ghidra's synthetic imports, not bodies in
        # the input image. Keep the native rows but do not turn them into RVAs.
        return [(address(row["source"], 10), address(row["destination"], 10))
                for row in rows if row["source_space"] == "ram"]
    with closing(sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)) as database:
        if kind == "bindiff":
            identities = database.execute(
                "SELECT p.hash, s.hash FROM metadata m "
                "JOIN file p ON p.id=m.file1 JOIN file s ON s.id=m.file2").fetchall()
            require(len(identities) == 1, "BinDiff must contain one comparison")
            for recorded, source in zip(identities[0], inputs):
                require(isinstance(recorded, str) and len(recorded) in (40, 64),
                        "unsupported BinDiff input hash")
                require(recorded.lower() == digest(source, "sha1" if len(recorded) == 40 else "sha256"),
                        "BinDiff input identity does not match the corpus")
            # SQLite stores unsigned native addresses in signed INTEGER cells.
            return [(address(a & ((1 << ADDRESS_BITS) - 1)),
                     address(b & ((1 << ADDRESS_BITS) - 1)))
                    for a, b in database.execute("SELECT address1, address2 FROM function")]
        require(kind == "diaphora", "unsupported result format")
        rows = database.execute("SELECT type, address, address2 FROM results").fetchall()
        require(all(category in ("best", "partial", "unreliable", "multimatch")
                    for category, _, _ in rows), "unknown Diaphora result category")
        return [(address(a, 16), address(b, 16)) for category, a, b in rows
                if category in ("best", "partial")]


def analysis_spaces(run: dict, result: Path, root: Path, inputs: list[Path]) -> list[dict]:
    """Read native analysis bases, never infer a relocation from scored matches."""
    kind = run["format"]
    if kind == "neverd":
        report = load(result)
        return [{"image_base": address(report[side]["image_base"]),
                 "entries": [address(row[0]) for row in report["functions"][side]]}
                for side in ("primary", "secondary")]
    if kind == "ghidra-vt":
        report = load(result)
        return [{"image_base": address(report[side + "_image_base"], 10),
                 "entries": [address(entry, 10) for entry in report[side + "_function_entries"]]}
                for side in ("source", "destination")]
    if kind == "bindiff":
        require(len(run["analyses"]) == 2, "BinDiff requires two retained analysis receipts")
        spaces = []
        for item, source in zip(run["analyses"], inputs):
            receipt = load(checked_artifact(item["receipt"], root))
            graph = checked_artifact(item["export"], root)
            require(receipt["schema"] == "neverd.benchmark.binexport.v1" and
                    receipt["complete"] is True and receipt["input_sha256"] == digest(source) and
                    receipt["export_sha256"] == digest(graph), "BinDiff analysis binding differs")
            native = binexport.inventory(graph)
            identity = native.pop("executable_id")
            require(len(identity) in (40, 64) and
                    identity.lower() == digest(source, "sha1" if len(identity) == 40 else "sha256"),
                    "BinExport input identity differs")
            spaces.append({"image_base": address(receipt["image_base"]), **native,
                           "ida_function_entries": len(receipt["function_entries"])})
        return spaces
    require(kind == "diaphora-native", "partial source scoring requires native analysis address evidence")
    receipt = load(result)
    spaces = []
    for side, source in zip(("primary", "secondary"), inputs):
        item = receipt[side]
        database = checked_artifact(item["database"], result.parent)
        declaration = checked_artifact(item["export_receipt"], result.parent)
        export = checked_diaphora_export(declaration, database, source)
        with closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as connection:
            entries = [address(row[0], 10) for row in connection.execute("SELECT address FROM functions")]
        spaces.append({"image_base": address(export["image_base"]), "entries": entries})
    return spaces


def normalize_pairs(pairs: list[tuple[int, int]], spaces: list[dict], case: dict) -> tuple[list, list]:
    require(len(spaces) == 2, "two native address spaces are required")
    bases = [address(case[side]["image_base"]) for side in ("primary", "secondary")]

    def translate(value, side):
        require(value >= spaces[side]["image_base"], "function lies below its native image base")
        return address(value - spaces[side]["image_base"] + bases[side])

    inventories = [{translate(value, side) for value in spaces[side]["entries"]} for side in range(2)]
    normalized = [(translate(a, 0), translate(b, 1)) for a, b in pairs]
    require(all(a in inventories[0] and b in inventories[1] for a, b in normalized),
            "accepted association is absent from its native function inventory")
    coverage = []
    for side, key in enumerate(("primary", "secondary")):
        labelled = {address(row[key]) for row in case["truth"]}
        discovered = inventories[side] & labelled
        coverage.append({"native_image_base": hex(spaces[side]["image_base"]),
                         "corpus_image_base": hex(bases[side]),
                         "discovered_functions": len(inventories[side]), "labelled_functions": len(labelled),
                         "inventory_source": spaces[side].get("inventory_source", "native-function-inventory"),
                         "vertex_types": spaces[side].get("vertex_types"),
                         "ida_function_entries": spaces[side].get("ida_function_entries"),
                         "discovered_labelled_functions": len(discovered),
                         "missing_labelled_functions": [hex(value) for value in sorted(labelled - discovered)]})
    return normalized, coverage


def evaluate(corpus_path: Path, run_path: Path) -> dict:
    corpus, run = load(corpus_path), load(run_path)
    for item in corpus.get("preparation_artifacts", []):
        checked_artifact(item, corpus_path.parent)
    require(run["schema"] == RUN_SCHEMA, "unsupported run schema")
    cases = [case for case in corpus_cases(corpus) if case["id"] == run["case"]]
    require(len(cases) == 1, "run must identify exactly one corpus case")
    case = cases[0]
    expected = truth_pairs(case["truth"])
    for item in case.get("truth_artifacts", []):
        checked_artifact(item, corpus_path.parent)
    inputs = [checked_artifact(case[side], corpus_path.parent) for side in ("primary", "secondary")]
    require(run["input_sha256"] == [digest(path) for path in inputs],
            "run input identities or comparison direction do not match the corpus")
    require(run["completed"] is True, "tool run did not complete")
    require(run["track"] in ("end-to-end", "common-input"), "unknown comparison track")
    if not case["truth_complete"]:
        require(run["track"] == "end-to-end",
                "partial source corpus has no qualified common-input representation")
    require(bool(run["tool"]["name"]) and bool(run["tool"]["version"]), "tool version is required")
    result = checked_artifact(run["result"], run_path.parent)
    for item in run["artifacts"]:
        checked_artifact(item, run_path.parent)
    pairs = read_pairs(run["format"], result, inputs)
    coverage = None
    if not case["truth_complete"]:
        pairs, coverage = normalize_pairs(pairs, analysis_spaces(run, result, run_path.parent, inputs), case)
    # Detect mutations during evaluation as well as stale receipts.
    for side in ("primary", "secondary"):
        checked_artifact(case[side], corpus_path.parent)
    checked_artifact(run["result"], run_path.parent)
    native = load(result) if run["format"] == "neverd" else None
    return {
        "schema": RESULT_SCHEMA, "case": case["id"], "category": case["category"],
        "tool": run["tool"], "track": run["track"], "format": run["format"],
        "corpus_sha256": digest(corpus_path), "run_sha256": digest(run_path),
        "evaluator_sha256": EVALUATOR_SHA256,
        "evaluator_components": {"benchmark_binary_diff.py": EVALUATOR_SHA256,
                                 "benchmark_binary_diff_binexport.py": binexport.READER_SHA256},
        "result_sha256": run["result"]["sha256"], "input_sha256": run["input_sha256"],
        "input_binding": "native-and-receipt" if run["format"] in ("neverd", "bindiff", "ghidra-vt", "diaphora-native") else "receipt",
        "metrics": score(expected, pairs) if case["truth_complete"] else score_partial(expected, pairs),
        "analysis_coverage": coverage,
        "native_coverage": native.get("coverage") if native else None,
        "native_discovery_complete": ([native[side].get("discovery_complete")
                                       for side in ("primary", "secondary")] if native else None),
        "completion_scope": run.get("completion_scope", "function-mapping"),
        "scope": ("Complete authored function truth." if case["truth_complete"] else
                  "Partial source-body correspondence; unlabelled pairs are unscored.") +
                 " No held-out, semantic-equivalence or timing qualification.",
        "metric_note": "A wrong pair can also miss a true pair; wrong and missed are not disjoint function counts.",
    }


FUNCTIONS = {
    "rotate": "return (x << 13) | (x >> 51);",
    "multiply": "return (x * 37) ^ (x >> 11);",
    "fold": "return (x & 255) + ((x >> 8) & 255) + ((x >> 16) & 255);",
    "clamp": "return x < 500 ? x : 500;",
    "alternate": "return (x & 1) ? (x + 0x1234) : (x ^ 0x5678);",
    "bound": "return (x & 255) < BOUND ? x + 5 : x - 9;",
    "combine": "return (x ^ (x << 7)) + (x >> 17) + 31;",
    "dispatch": "return x == 0x7788 ? rotate(x) : multiply(x + 19);",
}


def reordered_source(reverse: bool) -> str:
    names = list(FUNCTIONS)
    ordered = list(reversed(names[:-1])) + names[-1:] if reverse else names
    text = "typedef unsigned long long u64;\n#define OWN __attribute__((noinline,used))\n"
    text += "\n".join(f"OWN u64 {name}(u64 x) {{ {FUNCTIONS[name]} }}" for name in ordered)
    text += "\nOWN u64 entry(u64 x) { return "
    text += " + ".join(f"{name}(x + {index})" for index, name in enumerate(names)) + "; }\n"
    return text


def linker_truth(path: Path, expected: dict[str, set[str]]) -> dict[str, int]:
    """Read only authored .text symbols from the retained LLD map."""
    owner = None
    result = {}
    for line in path.read_text().splitlines():
        fields = line.split(maxsplit=4)
        if len(fields) != 5 or not re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
            continue
        token = fields[4]
        if ":(" in token:
            candidate, section = token.rsplit(":(", 1)
            owner = Path(candidate).name if section == ".text)" else None
        elif owner in expected and token in expected[owner]:
            key = owner + ":" + token
            require(key not in result, f"duplicate linker-map identity: {key}")
            result[key] = int(fields[0], 16)
    identities = {owner + ":" + name for owner, names in expected.items() for name in names}
    require(result.keys() == identities, f"linker map is missing authored functions: {identities - result.keys()}")
    require(len(set(result.values())) == len(result), "fixture contains folded function addresses")
    return result


def tool_record(path: Path) -> dict:
    return {"path": str(path), "sha256": digest(path), "version": subprocess.check_output(
        [str(path), "--version"], text=True, stderr=subprocess.STDOUT).strip()}


def generate(args: argparse.Namespace) -> int:
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    # Preserve argv[0]: LLVM's ld.lld symlink selects the ELF driver in lld.
    compiler, linker, strip = (path.absolute() for path in (args.clang, args.linker, args.strip))
    tools = {"clang": tool_record(compiler), "linker": tool_record(linker), "strip": tool_record(strip)}
    manifest = {"schema": CORPUS_SCHEMA, "origin": "NeverD-authored C; truth from retained LLD maps",
                "architecture": "x86-64", "format": "ELF", "tools": tools,
                "generator_sha256": digest(Path(__file__)), "cases": []}
    for name in ("stripped-reorder", "stripped-reorder-bound-change", "duplicate-symbol-reorder"):
        folder = output / name
        private, inputs = folder / "truth", folder / "inputs"
        private.mkdir(parents=True)
        inputs.mkdir()
        commands, truth = [], {}
        def execute(command: list[str]) -> None:
            commands.append(command)
            with (private / "build.log").open("a") as log:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        if name == "duplicate-symbol-reorder":
            sources = {
                "one": "static __attribute__((noinline)) int helper(int x) { return x * 7 + 11; }\nint one(int x) { return helper(x) + 5; }\n",
                "two": "static __attribute__((noinline)) int helper(int x) { return (x ^ 93) + 17; }\nint two(int x) { return helper(x) - 2; }\n",
                "entry": "int one(int); int two(int); int entry(int x) { return one(x) + two(x); }\n",
            }
            expected = {"one.o": {"one", "helper"}, "two.o": {"two", "helper"}, "entry.o": {"entry"}}
        else:
            sources = {"before": reordered_source(False), "after": reordered_source(True)}
            expected = {"unit.o": set(FUNCTIONS) | {"entry"}}
        for source_name, content in sources.items():
            (private / (source_name + ".c")).write_text(content)
        for side in ("before", "after"):
            object_dir = private / side
            object_dir.mkdir()
            objects = []
            names = ["two", "one", "entry"] if side == "after" else ["one", "two", "entry"]
            if name != "duplicate-symbol-reorder":
                names = [side]
            for source_name in names:
                obj = object_dir / (source_name + ".o" if name == "duplicate-symbol-reorder" else "unit.o")
                bound = "17" if side == "after" and name.endswith("bound-change") else "16"
                execute([str(compiler), "--target=x86_64-unknown-linux-gnu", "-O2", "-ffreestanding",
                         "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                         "-DBOUND=" + bound, "-c", str(private / (source_name + ".c")), "-o", str(obj)])
                objects.append(str(obj))
            unstripped, map_path = private / (side + ".unstripped"), private / (side + ".map")
            execute([str(linker), "-e", "entry", "--build-id=none", "-Map=" + str(map_path),
                     *objects, "-o", str(unstripped)])
            truth[side] = linker_truth(map_path, expected)
            if name == "duplicate-symbol-reorder":
                shutil.copyfile(unstripped, inputs / (side + ".elf"))
            else:
                execute([str(strip), "--strip-all", "-o", str(inputs / (side + ".elf")), str(unstripped)])
        rows = [{"id": identity, "primary": hex(truth["before"][identity]),
                 "secondary": hex(truth["after"][identity])} for identity in sorted(truth["before"])]
        truth_pairs(rows)
        require(any(row["primary"] != row["secondary"] for row in rows), "fixture did not reorder functions")
        manifest["cases"].append({"id": name, "category": name, "truth_complete": True, "truth": rows,
                                  "primary": artifact(inputs / "before.elf", output),
                                  "secondary": artifact(inputs / "after.elf", output),
                                  "commands": commands,
                                  "evidence": [artifact(path, output) for path in sorted(private.rglob("*")) if path.is_file()]})
    save(output / "manifest.json", manifest)
    print(output / "manifest.json")
    return 0


def run_neverd(args: argparse.Namespace) -> int:
    if __package__:
        from . import benchmark_binary_diff_external as external
    else:
        import benchmark_binary_diff_external as external
    corpus_path = args.corpus.resolve()
    corpus = load(corpus_path)
    cases = corpus_cases(corpus)
    executable, library = args.neverd.absolute(), args.library.absolute()
    require(sys.platform.startswith("linux") and library.name == "libneverd.so",
            "run requires Linux and an explicit libneverd.so runtime")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent), NEVERD_THREADS=str(args.threads),
               DEBUGINFOD_URLS="")
    version_stage = external.stage(output, "version", [str(executable), "--version"], env, args.timeout)
    require(version_stage["error"] is None and version_stage["exit"] == 0,
            "runtime version probe failed; see version.json")
    version = (output / "version.log").read_text(encoding="utf-8").strip()
    require(bool(version), "runtime version probe returned no version")
    runtime_files = (executable, library, Path(__file__).resolve(),
                     Path(binexport.__file__).resolve(), Path(external.__file__).resolve())
    runtime = [artifact(path, output) for path in runtime_files]
    results = []
    for case in cases:
        inputs = [checked_artifact(case[side], corpus_path.parent) for side in ("primary", "secondary")]
        folder = output / case["id"]
        folder.mkdir()
        report = folder / "result.nddiff"
        command = [str(executable), "diff", "--structural", "-a", str(inputs[0]), "-b", str(inputs[1]),
                   "--save", str(report), "--json"]
        cancelled = False
        try:
            outcome = external.stage(folder, "compare", command, env, args.timeout)
        except KeyboardInterrupt:
            outcome = load(folder / "compare.json")
            cancelled = True
        exit_code, error = outcome["exit"], outcome["error"]
        native = None
        try:
            if report.is_file():
                native = load(report)
                require(isinstance(native, dict), "native report must be an object")
        except (ValueError, OSError) as failure:
            error = f"invalid native report: {failure}" if error is None else f"{error}; invalid native report: {failure}"
            native = None
        completed = (error is None and exit_code in (0, 2, 3) and native is not None and
                     native.get("state") == "complete")
        run = {"schema": RUN_SCHEMA, "case": case["id"], "format": "neverd", "track": "end-to-end",
               "runner_sha256": EVALUATOR_SHA256,
               "tool": {"name": "NeverD", "version": version}, "input_sha256": [item["sha256"] for item in (case["primary"], case["secondary"])],
               "command": command, "configuration": {"threads": args.threads,
                                                        "stage_timeout_seconds": args.timeout}, "exit": exit_code,
               "wall_seconds": outcome["wall_seconds"], "resources": outcome["resources"],
               "completed": completed, "completion_scope": "function-mapping",
               "native_coverage": native.get("coverage") if native else None,
               "error": error, "artifacts": [artifact(path, folder) for path in runtime_files],
               "result": artifact(report, folder) if report.exists() else None}
        run_path = folder / "run.json"
        save(run_path, run)
        try:
            result = evaluate(corpus_path, run_path)
        except (ValueError, KeyError, OSError) as failure:
            result = {"case": case["id"], "error": str(failure)}
        save(folder / "evaluation.json", result)
        results.append(result)
        print(case["id"], metric_summary(result), flush=True)
        if cancelled:
            raise KeyboardInterrupt
    for item in runtime:
        checked_artifact(item, output)
    save(output / "results.json", {"schema": RESULT_SCHEMA, "cases": results, "runtime": runtime})
    if any("metrics" not in result for result in results):
        return 2
    return 0 if all(mapping_passed(result) for result in results) else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate_parser = commands.add_parser("generate", help="build owned x64 ELF diagnostics with explicit tools")
    for flag in ("output-dir", "clang", "linker", "strip"):
        generate_parser.add_argument("--" + flag, required=True, type=Path)
    run_parser = commands.add_parser("run", help="run a frozen Linux NeverD CLI on every corpus case")
    for flag in ("corpus", "neverd", "library", "output-dir"):
        run_parser.add_argument("--" + flag, required=True, type=Path)
    run_parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    run_parser.add_argument("--threads", type=int, default=1)
    evaluate_parser = commands.add_parser("evaluate", help="score a hash-bound retained tool result")
    for flag in ("corpus", "run", "output"):
        evaluate_parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "generate":
            return generate(args)
        if args.command == "run":
            require(math.isfinite(args.timeout) and args.timeout > 0 and args.threads > 0,
                    "timeout must be finite and positive; threads must be positive")
            return run_neverd(args)
        result = evaluate(args.corpus.resolve(), args.run.resolve())
        save(args.output, result)
        print(json.dumps(result["metrics"], sort_keys=True))
        return 0 if mapping_passed(result) else 1
    except KeyboardInterrupt:
        print("binary diff benchmark: cancelled", file=sys.stderr)
        return 130
    except (ValueError, KeyError, TypeError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(f"binary diff benchmark: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
