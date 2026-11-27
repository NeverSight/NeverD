#!/usr/bin/env python3
"""Summarize matched warm launches, keeping the phases of each slow launch together."""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import random
import statistics


QUIET_BACKGROUND_MAX = 0.35


def percentile(values, fraction):
    return sorted(values)[math.ceil(fraction * len(values)) - 1]


def load_class(load):
    before, during, after = (load[k] for k in ("before", "during", "after"))
    if max(before, after) <= QUIET_BACKGROUND_MAX and during <= 0.70:
        measured = "quiet" if load.get("quiet_settled", True) else "unclassified"
    elif min(before, after) >= 0.85 and during >= 0.90:
        measured = "busy"
    else:
        return "unclassified"
    return measured if load["condition"] in ("ambient", measured) else "unclassified"


def paired_p95(cohorts, field, draws):
    # A draw keeps each before/after pair together. Resampling whole rounds
    # and adjacent pairs accounts for the order and local bursts of load.
    rng = random.Random(49371)
    groups = [[(a[field], b[field]) for a, b in cohort] for cohort in cohorts]
    chunks = [[rows[i:i + 2] for i in range(0, len(rows), 2)] for rows in groups]
    differences = []
    for _ in range(draws):
        sample = [pair for group in rng.choices(chunks, k=len(chunks))
                  for chunk in rng.choices(group, k=len(group)) for pair in chunk]
        left, right = zip(*sample)
        differences.append(percentile(right, 0.95) - percentile(left, 0.95))
    return [percentile(differences, f) for f in (0.025, 0.975)]


def tail_and_middle(records, rank):
    ordered = sorted(records, key=lambda item: item[rank])
    n = len(ordered)
    # Compare the slowest five percent with the central ten percent of the
    # same ranking, preserving the observations belonging to each launch.
    tail = ordered[-math.ceil(n * 0.05):]
    typical = ordered[min(n - 1, int(n * 0.45)):max(1, math.ceil(n * 0.55))]
    return tail, typical


def tail_components(records):
    tail, typical = tail_and_middle(records, "wall_ms")
    fields = ["pre_main_ms", "main_ms", "teardown_ms", "exit_to_observer_ms", "wall_ms",
              "spawn_to_birth_ms"]
    excess = {key: statistics.mean(row[key] for row in tail)
              - statistics.mean(row[key] for row in typical) for key in fields}
    excess["birth_to_main_ms"] = excess["pre_main_ms"] - excess["spawn_to_birth_ms"]
    return excess


def entry_bounds(row):
    # Only the stop closes the current runnable interval. The live sample
    # precedes the stop and the parent timestamp follows it. Bound how much
    # the child could have accrued between main entry and those observations.
    wall = row["birth_to_main_ms"]
    gate = row["probe_to_gate_ms"]
    delay = gate + row["gate_to_observer_ms"]
    cpu = row["cpu_at_entry_ms"]
    at_stop = row["runnable_at_stop_ms"]
    cpu_low = max(0, cpu - gate, row["cpu_at_stop_ms"] - delay)
    cpu_high = min(wall, cpu, row["cpu_at_stop_ms"])
    runnable_low = max(0, at_stop - delay, row["runnable_at_entry_ms"] - gate)
    runnable_high = min(wall, at_stop)
    if cpu_low > cpu_high + 1e-6 or max(runnable_low, cpu_low) > runnable_high + 1e-6:
        raise ValueError("entry counters exceed their observation window")
    return {"cpu_ms": (cpu_low, cpu_high),
            "ready_ms": (max(0, runnable_low - cpu_high), max(0, runnable_high - cpu_low)),
            "other_ms": (wall - runnable_high, wall - max(runnable_low, cpu_low))}


def entry_tail(records):
    tail, middle = tail_and_middle(records, "birth_to_main_ms")
    slow, typical = ([entry_bounds(row) for row in group] for group in (tail, middle))
    # Subtract opposite endpoints. A delayed observer widens the interval.
    excess = {key: [statistics.mean(v[key][i] for v in slow)
                    - statistics.mean(v[key][1 - i] for v in typical) for i in (0, 1)]
              for key in slow[0]}
    fields = ("birth_to_main_ms", "pageins", "bytes_read", "faults", "cow_faults")
    return {"slowest_mean": {k: statistics.mean(r[k] for r in tail) for k in fields},
            "middle_mean": {k: statistics.mean(r[k] for r in middle) for k in fields},
            "excess_bounds_ms": excess}


def summarize_group(cohorts, marked, draws):
    groups = list(cohorts.values())
    pairs = [pair for group in groups for pair in group]
    report = {"pairs": len(pairs), "rounds": len(groups), "metrics": {}}
    for field in pairs[0][0]:
        if not field.endswith("_ms") and field != "instructions":
            continue
        left, right = ([pair[i][field] for pair in pairs] for i in (0, 1))
        change = percentile(right, 0.95) - percentile(left, 0.95)
        metric = {"before": {"p50": statistics.median(left), "p95": percentile(left, 0.95)},
                  "after": {"p50": statistics.median(right), "p95": percentile(right, 0.95)},
                  "p95_difference": change,
                  "by_round": {k: percentile([b[field] for _, b in c], 0.95)
                               - percentile([a[field] for a, _ in c], 0.95)
                               for k, c in cohorts.items()}}
        if len(groups) > 1 and field in ("wall_ms", "lifetime_ms", "cpu_ms", "birth_to_main_ms"):
            metric["p95_difference_ci95"] = paired_p95(groups, field, draws)
        report["metrics"][field] = metric
    if marked and "runnable_at_stop_ms" in pairs[0][0]:
        report["entry_tail"] = {key: entry_tail([p[i] for p in pairs])
                                for i, key in enumerate(("before", "after"))}
    elif marked:
        report["slow_tail_minus_typical_ms"] = {
            key: tail_components([p[i] for p in pairs]) for i, key in enumerate(("before", "after"))
        }
    return report


def summarize(path, draws=2000):
    setup = None
    loads = {}
    samples = {}
    with path.open() as source:
        for line in source:
            row = json.loads(line)
            if row["event"] == "setup" and setup is None:
                setup = row
            elif row["event"] == "load":
                key = row["round"], row["condition"]
                if key in loads:
                    raise ValueError("duplicate load record")
                loads[key] = row
            elif row["event"] == "sample":
                key = row["round"], row["condition"], row["kind"], row["other"], row["pair"], row["side"]
                if key in samples:
                    raise ValueError("duplicate launch")
                samples[key] = row
            else:
                raise ValueError("unexpected measurement record")
    if setup is None or not loads:
        raise ValueError("missing setup or completed rounds")
    conditions = {condition for _, condition in loads}
    if conditions not in ({"ambient"}, {"quiet", "busy"}):
        raise ValueError("unexpected load conditions")
    expected = {(i, condition) for i in range(setup["rounds"]) for condition in conditions}
    if loads.keys() != expected:
        raise ValueError("incomplete measurement rounds")

    grouped = defaultdict(dict)
    for (i, condition), observation in sorted(loads.items()):
        load = load_class(observation)
        for kind in ("stock", "marked"):
            for other in ("A2", "B"):
                prefix = i, condition, kind, other
                # Earlier records used the primary A as the left side of A/A.
                independent_control = "A1" in setup.get("sdk_sha256", {}) and other == "A2"
                left = "A1" if independent_control else "A"
                try:
                    group = [(samples.pop((*prefix, j, left)), samples.pop((*prefix, j, other)))
                             for j in range(0, setup["pairs"], 2 if other == "A2" else 1)]
                except KeyError as error:
                    raise ValueError(f"missing launch in {prefix}") from error
                grouped[load, kind, other][f"{i}/{condition}"] = group
    if samples:
        raise ValueError("unexpected launch outside the paired rounds")
    return {"setup": setup, "loads": list(loads.values()),
            "groups": {"/".join(key): summarize_group(cohorts, key[1] == "marked", draws)
                       for key, cohorts in grouped.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    report = summarize(args.samples)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    print("Load / executable / comparison       Metric             Pairs    Before P95    After P95   95% CI for change (ms)")
    for label, data in report["groups"].items():
        metric = "birth_to_main_ms" if "entry_tail" in data else "wall_ms"
        wall = data["metrics"][metric]
        ci = wall.get("p95_difference_ci95")
        interval = f"[{ci[0]:.2f}, {ci[1]:.2f}]" if ci else "one round"
        print(f"{label:<37} {metric:<17} {data['pairs']:>5} {wall['before']['p95']:>12.2f} "
              f"{wall['after']['p95']:>12.2f}   {interval}")


if __name__ == "__main__":
    main()
