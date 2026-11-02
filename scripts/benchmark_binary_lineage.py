#!/usr/bin/env python3
"""Qualify offline lineage scale with retained native comparison fixtures.

Generate a chain with shortcuts, a cycle and a reverse comparison. Every
authored function has an independent expected address and identity. Reuse the
same hashed corpus when comparing builds; generation time is not a lineage
measurement. This is a native synthetic workload, not a matching-quality or
competitive benchmark. Resource refusals fail the run without shrinking input.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FUNCTIONS = 1000
DEFAULT_SNAPSHOTS = 16
DEFAULT_REPEATS = 3
DEFAULT_RELEASES = 10000
DEFAULT_TIMEOUT = 180.0
FUNCTION_STRIDE = 16
INVENTORY_PAGE_SIZE = 200
TRACE_PAGE_SIZE = 1
HASH_BLOCK_BYTES = 1024 * 1024
# Admit the small edit request while refusing the much larger retained corpus.
RELEASE_REFUSAL_WORK = 1024
MANIFEST_SCHEMA = "neverd.benchmark.lineage.corpus.v1"
RESULT_SCHEMA = "neverd.benchmark.lineage.result.v1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(HASH_BLOCK_BYTES), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def bindings_digest() -> str:
    package = ROOT / "pluginsdk" / "python" / "neverd_plugin"
    inventory = {str(path.relative_to(package)): digest(path) for path in sorted(package.rglob("*.py"))}
    return hashlib.sha256(json.dumps(inventory, sort_keys=True).encode()).hexdigest()


def execute(command: list[str], log: Path, timeout: float, *,
            data: bytes | None = None, allowed: tuple[int, ...] = (0,)) -> None:
    with log.open("wb") as out, log.with_suffix(".stderr").open("wb") as err:
        process = subprocess.Popen(command, stdin=subprocess.PIPE if data is not None else None,
                                   stdout=out, stderr=err, start_new_session=os.name == "posix")
        try:
            process.communicate(data, timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.communicate()
            raise RuntimeError(f"command exceeded {timeout:g}s; see {log}") from None
    require(process.returncode in allowed,
            f"command exited {process.returncode}; see {log.with_suffix('.stderr')}")


def pair_inventory(snapshots: int) -> list[tuple[int, int]]:
    require(snapshots >= 3, "at least three snapshots are required")
    return sorted(set([(i, i + 1) for i in range(snapshots - 1)] +
                      [(0, snapshots // 2), (snapshots // 2, snapshots - 1),
                       (0, snapshots - 1), (1, 0)]))


def distances(snapshots: int, pairs: list[list[int]]) -> list[int]:
    adjacent = [set() for _ in range(snapshots)]
    for left, right in pairs:
        require(0 <= left < snapshots and 0 <= right < snapshots, "invalid snapshot edge")
        adjacent[left].add(right)
        adjacent[right].add(left)
    result = [-1] * snapshots
    result[0] = 0
    queue = deque([0])
    while queue:
        left = queue.popleft()
        for right in adjacent[left]:
            if result[right] == -1:
                result[right] = result[left] + 1
                queue.append(right)
    require(-1 not in result, "fixture graph must be connected")
    return result


def generate(args: argparse.Namespace) -> None:
    require(args.functions > 0, "function count must be positive")
    pairs = pair_inventory(args.snapshots)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    producer = {"neverd": str(args.neverd), "neverd_sha256": digest(args.neverd),
                "compiler": str(args.compiler), "compiler_sha256": digest(args.compiler)}
    versions = []
    for version in range(args.snapshots):
        source = [".text"]
        for index in range(args.functions):
            name = f"lineage_function_{index:08d}"
            source.extend([".p2align 4", f".globl {name}", f".type {name}, @function",
                           f"{name}:", f"movl ${((version << 16) ^ index) & 0xffffffff}, %eax",
                           "ret", f".size {name}, .-{name}"])
        source.append('.section .note.GNU-stack,"",@progbits')
        path = output / f"snapshot-{version:04d}.o"
        execute([str(args.compiler), "-target", "x86_64-unknown-linux-gnu", "-c",
                 "-x", "assembler", "-", "-o", str(path)],
                output / f"compile-{version:04d}.log", args.timeout,
                data=("\n".join(source) + "\n").encode())
        versions.append({"file": path.name, "sha256": digest(path)})
    expected_functions = {(index * FUNCTION_STRIDE, f"lineage_function_{index:08d}")
                          for index in range(args.functions)}
    expected_addresses = {address for address, _ in expected_functions}
    reports, originals = [], []
    for left, right in pairs:
        path = output / f"comparison-{left:04d}-{right:04d}.nddiff"
        execute([str(args.neverd), "diff", "--structural", "-a",
                 str(output / versions[left]["file"]), "-b",
                 str(output / versions[right]["file"]), "--json"],
                path, args.timeout, allowed=(0, 2))
        raw = path.read_bytes().decode("utf-8")
        report = json.loads(raw)
        require(report["state"] == "complete" and not report["ambiguities"],
                f"incomplete or ambiguous fixture comparison: {path}")
        for side, version in (("primary", left), ("secondary", right)):
            functions = report["functions"][side]
            require(report[side]["sha256"] == versions[version]["sha256"] and
                    len(functions) == args.functions and
                    {(int(row[0], 0), row[2]) for row in functions} == expected_functions,
                    f"fixture inventory mismatch: {path} {side}")
        require(len(report["matches"]) == args.functions and
                {(int(row["primary"], 0), int(row["secondary"], 0))
                 for row in report["matches"]} == {(address, address) for address in expected_addresses},
                f"fixture associations disagree with authored identities: {path}")
        reports.append({"file": path.name, "sha256": digest(path), "pair": [left, right]})
        originals.append(raw)
    collection = output / "collection.ndlineage"
    write_json(collection, {"schema": "neverd.bindiff.lineage", "schema_version": 1,
                            "reports": originals})
    require(digest(args.neverd) == producer["neverd_sha256"] and
            digest(args.compiler) == producer["compiler_sha256"], "fixture producer changed during generation")
    write_json(output / "manifest.json", {
        "schema": MANIFEST_SCHEMA, "generator_sha256": digest(Path(__file__)),
        "producer": producer,
        "functions_per_snapshot": args.functions, "versions": versions, "reports": reports,
        "collection": {"file": collection.name, "sha256": digest(collection),
                       "size_bytes": collection.stat().st_size}})


def corpus(path: Path) -> tuple[dict, str]:
    manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["schema"] == MANIFEST_SCHEMA, "unknown corpus schema")
    for item in [*manifest["versions"], *manifest["reports"], manifest["collection"]]:
        leaf = Path(item["file"])
        require(leaf.name == str(leaf), "corpus paths must be local filenames")
        require(digest(path / leaf) == item["sha256"], f"corpus digest mismatch: {leaf}")
    require(manifest["functions_per_snapshot"] > 0, "empty function inventory")
    require(len({v["sha256"] for v in manifest["versions"]}) == len(manifest["versions"]),
            "fixture snapshots must have distinct bytes")
    require(sorted(tuple(r["pair"]) for r in manifest["reports"]) ==
            pair_inventory(len(manifest["versions"])), "unexpected fixture topology")
    return manifest, (path / manifest["collection"]["file"]).read_bytes().decode("utf-8")


def generate_releases(args: argparse.Namespace) -> None:
    """Retain a sparse fork/merge DAG independently described by index pairs."""
    require(args.releases >= 4, "at least four release records are required")
    manifest, document = corpus(args.corpus)
    require("release_graph" not in manifest, "input already contains a release workload")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(ROOT / "pluginsdk" / "python"))
    from neverd_plugin import Lineage
    from neverd_plugin.ffi import HostAPI
    library_hash = digest(args.library)
    directory = os.add_dll_directory(str(args.library.parent)) if hasattr(os, "add_dll_directory") else None
    with Lineage(host=HostAPI(ctypes.CDLL(str(args.library)))) as graph:
        graph.load(document)
        versions = [r["id"] for page in graph.pages("versions", limit=INVENTORY_PAGE_SIZE)
                    for r in page["items"]]
    if directory is not None:
        directory.close()
    identifiers = [f"release-{i:08d}" for i in range(args.releases)]
    edges = [(parent, child) for child in range(1, args.releases)
             for parent in sorted({(child - 1) // 2, max(0, child - 3)})]
    parents: list[list[str]] = [[] for _ in identifiers]
    for parent, child in edges:
        parents[child].append(identifiers[parent])
    records = [{"id": identifier, "label": f"Release {i}", "branch": f"branch-{i % 3}",
                "version": versions[i % len(versions)], "published_at": None,
                "parents": parents[i]} for i, identifier in enumerate(identifiers)]
    candidate = json.loads(document)
    candidate.update(schema_version=2, releases=list(reversed(records)))
    for item in [*manifest["versions"], *manifest["reports"]]:
        (output / item["file"]).write_bytes((args.corpus / item["file"]).read_bytes())
    collection = output / "collection.ndlineage"
    write_json(collection, candidate)
    manifest.update(generator_sha256=digest(Path(__file__)),
                    release_generator_library_sha256=library_hash,
                    release_graph={"ids": identifiers, "edges": edges, "records": records},
                    collection={"file": collection.name, "sha256": digest(collection),
                                "size_bytes": collection.stat().st_size})
    require(digest(args.library) == library_hash, "generator library changed")
    write_json(output / "manifest.json", manifest)


def sample(args: argparse.Namespace) -> None:
    manifest, document = corpus(args.corpus)
    manifest_hash = digest(args.corpus / "manifest.json")
    harness_hash = digest(Path(__file__))
    bindings_hash = bindings_digest()
    library_hash = digest(args.library)
    sys.path.insert(0, str(ROOT / "pluginsdk" / "python"))
    from neverd_plugin import Lineage, NeverDError
    from neverd_plugin.ffi import HostAPI

    dll_directory = os.add_dll_directory(str(args.library.parent)) if hasattr(os, "add_dll_directory") else None
    host = HostAPI(ctypes.CDLL(str(args.library)))
    timings: dict[str, float] = {}

    def timed(name, operation):
        started = time.perf_counter()
        result = operation()
        timings[name] = time.perf_counter() - started
        return result

    count = manifest["functions_per_snapshot"]
    versions = len(manifest["versions"])
    reports = len(manifest["reports"])
    distances_by_index = distances(versions, [r["pair"] for r in manifest["reports"]])
    with Lineage(host=host) as graph:
        timed("cold_load_seconds", lambda: graph.load(document))
        summary = graph.summary()
        require(all(summary[key] == value for key, value in {
            "versions": versions, "reports": reports, "nodes": versions * count,
            "edges": reports * count, "conflicts": 0, "incomplete_reports": 0}.items()),
            "loaded graph disagrees with the independently authored inventory")
        require(not summary["native_authenticated"], "offline evidence claimed native authentication")
        revision = summary["revision"]

        def rows(kind, **options):
            return [row for page in graph.pages(kind, revision=revision,
                    limit=INVENTORY_PAGE_SIZE, **options) for row in page["items"]]

        version_rows = rows("versions")
        indices = {v["sha256"]: i for i, v in enumerate(manifest["versions"])}
        require(len(version_rows) == versions and
                {v["sha256"] for v in version_rows} == set(indices), "snapshot inventory mismatch")
        by_id = {v["id"]: indices[v["sha256"]] for v in version_rows}
        start = next(v["id"] for v in version_rows if by_id[v["id"]] == 0)
        functions = timed("function_inventory_seconds", lambda: rows("functions", version=start))
        require(len(functions) == count and
                {int(f["address"], 0) for f in functions} ==
                {i * FUNCTION_STRIDE for i in range(count)}, "paged function inventory mismatch")
        query = {"version": start, "address": "0x0", "revision": revision, "limit": TRACE_PAGE_SIZE}
        first = timed("first_trace_page_seconds", lambda: graph.query("trace_nodes", **query))
        require(first["total"] == versions, "first trace total mismatch")
        nodes = timed("cached_trace_pages_seconds", lambda: [row for page in
            graph.pages("trace_nodes", **query) for row in page["items"]])
        require(first["items"] == nodes[:TRACE_PAGE_SIZE], "trace cache changed the first page")
        require(len(nodes) == versions and
                {(by_id[n["version"]], int(n["address"], 0), n["distance"]) for n in nodes} ==
                {(i, 0, distance) for i, distance in enumerate(distances_by_index)},
                "trace reachability or shortest distances disagree with the independent graph")
        node_versions = {n["node"]: by_id[n["version"]] for n in nodes}
        edges = timed("cached_trace_edges_seconds", lambda: rows("trace_edges", version=start, address="0x0"))
        require(Counter((node_versions[e["primary"]], node_versions[e["secondary"]], e["report"])
                        for e in edges) == Counter((r["pair"][0], r["pair"][1], r["sha256"])
                        for r in manifest["reports"]), "parallel, reverse or non-tree evidence was lost")
        require(not rows("trace_conflicts", version=start, address="0x0"), "unexpected trace conflicts")
        try:
            graph.query("trace_nodes", **query, max_work=1)
        except NeverDError:
            require(graph.status()["operation_state"] == "resource_limited", "wrong budget refusal")
        else:
            raise ValueError("trace cache bypassed the current work budget")

        def check_other_seeds():
            for index in sorted({count // 2, count - 1} - {0}):
                address = index * FUNCTION_STRIDE
                selected = {"version": start, "address": hex(address)}
                other_nodes = rows("trace_nodes", **selected)
                require(len(other_nodes) == versions and
                        {(by_id[n["version"]], int(n["address"], 0), n["distance"]) for n in other_nodes} ==
                        {(i, address, distance) for i, distance in enumerate(distances_by_index)},
                        "changing the trace seed reused another function or lost a large-index node")
                other_versions = {n["node"]: by_id[n["version"]] for n in other_nodes}
                other_edges = rows("trace_edges", **selected)
                require(Counter((other_versions[e["primary"]], other_versions[e["secondary"]], e["report"])
                                for e in other_edges) ==
                        Counter((r["pair"][0], r["pair"][1], r["sha256"]) for r in manifest["reports"]),
                        "middle or last function lost original association evidence")
                require(not rows("trace_conflicts", **selected), "unexpected conflict after changing trace seed")

        timed("other_seed_traces_seconds", check_other_seeds)
        if "release_graph" in manifest:
            expected = manifest["release_graph"]
            timeline = timed("release_inventory_seconds", lambda: rows("releases"))
            require([r["id"] for r in timeline] == expected["ids"],
                    "release timeline disagrees with authored parent-before-child IDs")
            require(all(r["provenance"] == "analyst_declared" and "parents" not in r for r in timeline),
                    "release provenance or bounded row contract changed")
            require(summary["releases"] == len(expected["ids"]) and
                    summary["release_links"] == len(expected["edges"]), "release counts disagree")
            for index in sorted({0, len(timeline) // 2, len(timeline) - 1}):
                identifier = expected["ids"][index]
                actual = rows("release_parents", release=identifier)
                require([r["id"] for r in actual] == [expected["ids"][a] for a, b in
                    expected["edges"] if b == index], "release parent inventory changed")
                actual = rows("release_children", release=identifier)
                require([r["id"] for r in actual] == [expected["ids"][b] for a, b in
                    expected["edges"] if a == index], "release child inventory changed")
            last = dict(expected["records"][-1])
            last.pop("parents")
            renamed = {**last, "label": "Renamed benchmark release"}
            saved_release = graph.export()
            budget_started = time.perf_counter()
            try:
                graph.edit_releases({"upsert": [renamed]},
                                    options={"max_work": RELEASE_REFUSAL_WORK})
            except NeverDError:
                timings["release_budget_refusal_seconds"] = time.perf_counter() - budget_started
                require(graph.status()["operation_state"] == "resource_limited" and
                        graph.export() == saved_release, "limited release edit changed retained data")
            else:
                raise ValueError("release edit bypassed work budget")
            release_stop = None

            def cancel_release(phase, _done, _total):
                nonlocal release_stop
                if phase == "lineage:commit":
                    release_stop = time.perf_counter()
                    graph.cancel()

            try:
                graph.edit_releases({"upsert": [renamed]}, progress=cancel_release)
            except NeverDError:
                returned = time.perf_counter()
                require(release_stop is not None and graph.status()["operation_state"] == "cancelled",
                        "release edit did not stop at the requested commit boundary")
                timings["release_commit_cancel_return_seconds"] = returned - release_stop
                require(graph.export() == saved_release, "cancelled release edit changed retained data")
            else:
                raise ValueError("cancelled release edit unexpectedly completed")
            timed("release_label_edit_seconds", lambda: graph.edit_releases({"upsert": [renamed]}))
            graph.edit_releases({"upsert": [last]})
            require(graph.summary()["revision"] == revision and graph.export() == saved_release,
                    "release label restoration lost parents or changed canonical identity")
        candidate = json.loads(document)
        candidate["versions"] = [{"id": start, "label": "Cancelled benchmark replacement", "branch": ""}]
        candidate_text = json.dumps(candidate)
        requested = None

        def cancel(phase, _done, _total):
            nonlocal requested
            if phase == "lineage:commit":
                requested = time.perf_counter()
                graph.cancel()

        try:
            graph.load(candidate_text, progress=cancel)
        except NeverDError:
            returned = time.perf_counter()
            require(requested is not None and graph.status()["operation_state"] == "cancelled",
                    "replacement did not reach the requested cancellation boundary")
            timings["commit_cancel_return_seconds"] = returned - requested
            require(graph.summary()["revision"] == revision, "cancelled metadata replacement was published")
        else:
            raise ValueError("cancelled replacement unexpectedly completed")
        timed("cached_source_reload_seconds", lambda: graph.load(document))
        require(graph.summary()["revision"] == revision, "identical reload changed graph identity")
        saved = timed("export_seconds", graph.export)
        exported = json.loads(saved)
        require(Counter(hashlib.sha256(r.encode()).hexdigest() for r in exported["reports"]) ==
                Counter(r["sha256"] for r in manifest["reports"]), "source bytes changed during export")
    if dll_directory is not None:
        dll_directory.close()
    require(digest(args.library) == library_hash, "library changed during sample")
    require(digest(args.corpus / "manifest.json") == manifest_hash and
            digest(Path(__file__)) == harness_hash and bindings_digest() == bindings_hash,
            "corpus manifest, harness or Python bindings changed during sample")
    peak_rss_kib = None
    if sys.platform.startswith("linux"):
        import resource
        peak_rss_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    print(json.dumps({"validated": True, "timings": timings, "summary": summary,
                      "corpus_manifest_sha256": manifest_hash, "harness_sha256": harness_hash,
                      "bindings_sha256": bindings_hash,
                      "library_sha256": library_hash, "peak_process_rss_kib": peak_rss_kib,
                      "load_average": list(os.getloadavg()) if hasattr(os, "getloadavg") else None}))


def run(args: argparse.Namespace) -> None:
    require(args.repeats > 0, "repeat count must be positive")
    manifest, _ = corpus(args.corpus)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest_hash = digest(args.corpus / "manifest.json")
    harness_hash = digest(Path(__file__))
    bindings_hash = bindings_digest()
    library_hash = digest(args.library)
    samples = []
    for index in range(args.repeats):
        path = output / f"sample-{index:03d}.json"
        execute([sys.executable, str(Path(__file__).resolve()), "_sample",
                 "--corpus", str(args.corpus), "--library", str(args.library)], path, args.timeout)
        value = json.loads(path.read_text(encoding="utf-8"))
        require(value["validated"] and value["library_sha256"] == library_hash and
                value["corpus_manifest_sha256"] == manifest_hash and
                value["harness_sha256"] == harness_hash and value["bindings_sha256"] == bindings_hash,
                "unvalidated sample or changed library, corpus manifest, harness or Python bindings")
        samples.append(value)
    require(digest(args.library) == library_hash, "library changed during measurement")
    require(digest(args.corpus / "manifest.json") == manifest_hash and
            digest(Path(__file__)) == harness_hash and bindings_digest() == bindings_hash,
            "corpus manifest, harness or Python bindings changed during measurement")
    corpus(args.corpus)
    require(all(s["summary"] == samples[0]["summary"] for s in samples), "samples changed graph identity")
    write_json(output / "result.json", {
        "schema": RESULT_SCHEMA, "workload": ("native-synthetic-release-fork-merge" if
                    "release_graph" in manifest else "native-synthetic-chain-shortcuts-reverse"),
        "corpus_manifest": str(args.corpus / "manifest.json"),
        "corpus_manifest_sha256": manifest_hash,
        "collection_sha256": manifest["collection"]["sha256"],
        "harness_sha256": harness_hash, "bindings_sha256": bindings_hash, "library": str(args.library),
        "library_sha256": library_hash, "platform": platform.platform(),
        "python": platform.python_version(), "cpu_count": os.cpu_count(),
        "load_options": {}, "inventory_page_size": INVENTORY_PAGE_SIZE, "trace_page_size": TRACE_PAGE_SIZE,
        "repeats": args.repeats, "samples": samples,
        "median_timings": {key: statistics.median(s["timings"][key] for s in samples)
                           for key in samples[0]["timings"]},
        "scope": "Fresh-process Python binding calls; corpus reading and library loading are outside timings. "
                 "RSS covers the whole process, including Python, corpus validation and output checks. "
                 "Cold means a fresh Lineage handle, not an empty OS file cache. "
                 "No matching-quality, desktop-latency, native semantic-proof or competitive claim."})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate_parser = commands.add_parser("generate", help="retain native fixtures and their independent graph")
    generate_parser.add_argument("--neverd", type=Path, required=True)
    generate_parser.add_argument("--compiler", type=Path, required=True)
    generate_parser.add_argument("--output-dir", type=Path, required=True)
    generate_parser.add_argument("--functions", type=int, default=DEFAULT_FUNCTIONS)
    generate_parser.add_argument("--snapshots", type=int, default=DEFAULT_SNAPSHOTS)
    generate_parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    release_parser = commands.add_parser("generate-releases", help="add a retained release fork/merge DAG")
    release_parser.add_argument("--library", type=Path, required=True)
    release_parser.add_argument("--corpus", type=Path, required=True)
    release_parser.add_argument("--output-dir", type=Path, required=True)
    release_parser.add_argument("--releases", type=int, default=DEFAULT_RELEASES)
    for name in ("run", "_sample"):
        child = commands.add_parser(name)
        child.add_argument("--library", type=Path, required=True)
        child.add_argument("--corpus", type=Path, required=True)
        if name == "run":
            child.add_argument("--output-dir", type=Path, required=True)
            child.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
            child.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = parser.parse_args()
    if hasattr(args, "timeout"):
        require(args.timeout > 0, "timeout must be positive")
    for name in ("neverd", "compiler", "library", "corpus"):
        if hasattr(args, name):
            setattr(args, name, getattr(args, name).resolve(strict=True))
    {"generate": generate, "generate-releases": generate_releases,
     "run": run, "_sample": sample}[args.command](args)


if __name__ == "__main__":
    main()
