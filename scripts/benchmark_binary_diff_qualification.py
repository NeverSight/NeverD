#!/usr/bin/env python3
"""Freeze and check a bounded NeverD function-mapping qualification.

The plan binds the protocol, corpus, executable, library and harness before its
new output directory exists. Hashes preserve evidence; they do not authenticate
timestamps, establish statistical independence or qualify unlabelled functions.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import math
from pathlib import Path
import sys

if __package__:
    from . import benchmark_binary_diff as bench
    from . import benchmark_binary_diff_external as external
else:
    import benchmark_binary_diff as bench
    import benchmark_binary_diff_external as external

PLAN_SCHEMA = "neverd.benchmark.diff.qualification-plan.v1"
PROTOCOL_SCHEMA = "neverd.benchmark.diff.heldout-protocol.v1"
QUALIFICATION_SCHEMA = "neverd.benchmark.diff.qualification.v1"
GATE_NAMES = {
    "minimum_labelled_pairs_per_case", "minimum_conditional_precision_per_case",
    "minimum_truth_recall_per_case", "minimum_macro_f1", "maximum_receipt_failures",
    "maximum_timeouts",
}


def protocol(path: Path) -> dict:
    value = bench.load(path)
    bench.require(value["schema"] == PROTOCOL_SCHEMA, "unsupported qualification protocol")
    bench.require(value["track"] == "end-to-end", "qualification requires the end-to-end track")
    bench.require(isinstance(value["scope"], str) and bool(value["scope"]), "protocol scope is required")
    identities = value["case_ids"]
    bench.require(isinstance(identities, list) and bool(identities) and
                  all(isinstance(item, str) and bool(item) for item in identities) and
                  len(identities) == len(set(identities)), "protocol case identities must be nonempty and unique")
    gates = value["gates"]
    bench.require(set(gates) == GATE_NAMES, "protocol must declare exactly the supported gates")
    for key in GATE_NAMES:
        number = gates[key]
        if key.startswith("maximum_"):
            bench.require(type(number) is int and number == 0, "v1 requires zero failures and timeouts")
        elif key == "minimum_labelled_pairs_per_case":
            bench.require(type(number) is int and number > 0, "labelled-pair minimum must be positive")
        else:
            bench.require(type(number) in (int, float) and math.isfinite(number) and 0 < number <= 1,
                          "quality minima must be finite and in (0, 1]")
    runtime = value["runtime"]
    bench.require(type(runtime["threads"]) is int and runtime["threads"] > 0, "threads must be positive")
    timeout = runtime["stage_timeout_seconds"]
    bench.require(type(timeout) in (int, float) and math.isfinite(timeout) and timeout > 0,
                  "stage timeout must be finite and positive")
    return value


def harness_files() -> list[Path]:
    return [Path(module.__file__).resolve() for module in (bench, bench.binexport, external)]


def freeze(args: argparse.Namespace) -> dict:
    output, plan = args.output_dir.resolve(), args.plan.resolve()
    bench.require(not output.exists(), "qualification output must not exist before freezing")
    value = protocol(args.protocol)
    corpus_path = args.corpus.resolve()
    corpus = bench.load(corpus_path)
    bench.require(corpus["schema"] == bench.PARTIAL_CORPUS_SCHEMA,
                  "v1 qualification requires partial source-body truth")
    cases = bench.corpus_cases(corpus)
    bench.require([case["id"] for case in cases] == value["case_ids"],
                  "corpus does not contain exactly the protocol cases in order")
    for item in corpus.get("preparation_artifacts", []):
        bench.checked_artifact(item, corpus_path.parent)
    for case in cases:
        bench.require(len(case["truth"]) >= value["gates"]["minimum_labelled_pairs_per_case"],
                      f"insufficient labelled pairs: {case['id']}")
        for item in [case["primary"], case["secondary"], *case["truth_artifacts"]]:
            bench.checked_artifact(item, corpus_path.parent)
    bench.require(args.library.name == "libneverd.so", "select the explicit libneverd.so runtime")
    record = {
        "schema": PLAN_SCHEMA, "frozen_utc": datetime.now(timezone.utc).isoformat(),
        "protocol": bench.artifact(args.protocol.resolve(), plan.parent),
        "corpus": bench.artifact(corpus_path, plan.parent),
        "candidate": [bench.artifact(path.resolve(), plan.parent) for path in (args.neverd, args.library)],
        "harness": [bench.artifact(path, plan.parent) for path in harness_files()],
        "qualifier": bench.artifact(Path(__file__).resolve(), plan.parent),
        "case_ids": [case["id"] for case in cases],
        "output_directory": bench.recorded_path(output, plan.parent),
        "scope": value["scope"],
    }
    bench.save(plan, record)
    return record


def checked_plan(path: Path) -> tuple[dict, dict, Path, Path]:
    plan = bench.load(path)
    bench.require(plan["schema"] == PLAN_SCHEMA, "unsupported qualification plan")
    declaration = protocol(bench.checked_artifact(plan["protocol"], path.parent))
    corpus = bench.checked_artifact(plan["corpus"], path.parent)
    bench.require([case["id"] for case in bench.corpus_cases(bench.load(corpus))] == plan["case_ids"],
                  "qualification case inventory changed")
    bench.require(plan["case_ids"] == declaration["case_ids"], "protocol case inventory differs")
    bench.require(plan["scope"] == declaration["scope"], "qualification scope changed")
    for item in [*plan["candidate"], *plan["harness"], plan["qualifier"]]:
        bench.checked_artifact(item, path.parent)
    bench.require([item["sha256"] for item in plan["harness"]] ==
                  [bench.digest(file) for file in harness_files()], "use the frozen harness to qualify")
    bench.require(plan["qualifier"]["sha256"] == bench.digest(Path(__file__)), "use the frozen qualifier")
    return plan, declaration, corpus, (path.parent / plan["output_directory"]).resolve()


def gates_passed(metrics: dict, gates: dict) -> bool:
    return (metrics["expected_pairs"] >= gates["minimum_labelled_pairs_per_case"] and
            metrics["precision"] is not None and
            metrics["precision"] >= gates["minimum_conditional_precision_per_case"] and
            metrics["recall"] is not None and metrics["recall"] >= gates["minimum_truth_recall_per_case"])


def qualify(plan_path: Path) -> dict:
    plan, declaration, corpus, output = checked_plan(plan_path)
    gates = declaration["gates"]
    cases, failures, timeouts = [], 0, 0
    expected_runtime = [item["sha256"] for item in [*plan["candidate"], *plan["harness"]]]
    preflight = {}
    try:
        version_path = output / "version.json"
        version = bench.load(version_path)
        preflight["receipt"] = bench.artifact(version_path, output)
        timeouts += version["error"] == "timeout"
        candidate = bench.checked_artifact(plan["candidate"][0], plan_path.parent)
        bench.require(version["command"] == [str(candidate), "--version"] and
                      version["exit"] == 0 and version["error"] is None,
                      "runtime version probe failed or differs from the plan")
        bench.require(version["timeout_seconds"] == declaration["runtime"]["stage_timeout_seconds"],
                      "runtime probe deadline differs")
    except (ValueError, KeyError, TypeError, OSError) as error:
        failures += 1
        preflight["error"] = str(error)
    for identity in plan["case_ids"]:
        folder = output / identity
        case = {"case": identity, "passed": False}
        try:
            run_path = folder / "run.json"
            run = bench.load(run_path)
            stage = bench.load(folder / "compare.json")
            timeouts += stage["error"] == "timeout"
            case["run"] = bench.artifact(run_path, output)
            case["stage"] = bench.artifact(folder / "compare.json", output)
            bench.require(run["case"] == identity and run["format"] == "neverd" and
                          run["track"] == "end-to-end", "run profile or case differs")
            bench.require([item["sha256"] for item in run["artifacts"]] == expected_runtime,
                          "run did not use the frozen candidate and harness")
            bench.require(run["configuration"]["threads"] == declaration["runtime"]["threads"],
                          "run thread setting differs")
            bench.require(run["configuration"]["stage_timeout_seconds"] ==
                          stage["timeout_seconds"] == declaration["runtime"]["stage_timeout_seconds"],
                          "run deadline differs")
            candidate = bench.checked_artifact(plan["candidate"][0], plan_path.parent)
            bench.require(Path(run["command"][0]).resolve() == candidate,
                          "comparison command did not use the frozen executable")
            bench.require(run["command"] == stage["command"] and run["exit"] == stage["exit"] and
                          run["error"] is None and stage["error"] is None and
                          run["exit"] in (0, 2, 3), "comparison stage failed or differs from receipt")
            result = bench.evaluate(corpus, run_path)
            case.update({"metrics": bench.metric_summary(result),
                         "evaluation": result, "passed": gates_passed(result["metrics"], gates)})
        except (ValueError, KeyError, TypeError, OSError) as error:
            failures += 1
            case["error"] = str(error)
        cases.append(case)
    # A missing/failed case contributes zero, never disappears from the macro denominator.
    macro_f1 = sum(case.get("metrics", {}).get("f1") or 0 for case in cases) / len(cases)
    return {
        "schema": QUALIFICATION_SCHEMA, "plan": bench.artifact(plan_path, output),
        "scope": declaration["scope"], "gates": gates, "cases": cases,
        "runtime_probe": preflight,
        "receipt_failures": failures, "timeouts": timeouts, "macro_f1": macro_f1,
        "passed": all(case["passed"] for case in cases) and not failures and not timeouts and
                  macro_f1 >= gates["minimum_macro_f1"],
        "limitations": "Partial source-body mapping only. Unlabelled pairs remain unscored; "
                        "this does not prove semantic equivalence, whole-program precision, "
                        "statistical independence, competitive superiority or performance.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("freeze", help="bind a protocol and candidate before a new run")
    for name in ("plan", "protocol", "corpus", "neverd", "library", "output-dir"):
        prepare.add_argument("--" + name, type=Path, required=True)
    run = commands.add_parser("run", help="execute the frozen plan and record all quality gates")
    run.add_argument("--plan", type=Path, required=True)
    replay = commands.add_parser("evaluate", help="recheck all retained runs without executing binaries")
    replay.add_argument("--plan", type=Path, required=True)
    replay.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "freeze":
            freeze(args)
            print(f"Frozen qualification plan: {args.plan}")
            return 0
        plan_path = args.plan.resolve()
        plan, declaration, corpus, output = checked_plan(plan_path)
        cancelled = False
        execution_error = None
        if args.command == "run":
            bench.require(not output.exists(), "qualification output already exists")
            runtime = declaration["runtime"]
            try:
                bench.run_neverd(argparse.Namespace(
                    corpus=corpus, neverd=bench.checked_artifact(plan["candidate"][0], plan_path.parent),
                    library=bench.checked_artifact(plan["candidate"][1], plan_path.parent), output_dir=output,
                    threads=runtime["threads"], timeout=runtime["stage_timeout_seconds"]))
            except KeyboardInterrupt:
                cancelled = True
            except (ValueError, KeyError, TypeError, OSError) as error:
                execution_error = str(error)
        result = qualify(plan_path)
        if execution_error is not None:
            result.update(execution_error=execution_error, passed=False)
        bench.save(output / "qualification.json" if args.command == "run" else args.output, result)
        print({key: result[key] for key in ("passed", "receipt_failures", "timeouts", "macro_f1")})
        return 130 if cancelled else 0 if result["passed"] else 1
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f"binary diff qualification: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
