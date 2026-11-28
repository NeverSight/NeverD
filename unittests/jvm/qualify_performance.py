"""Measure the public JVM SDK on a deterministic JAR assembled from owned fixtures."""

import argparse
import base64
import ctypes
import hashlib
import json
from pathlib import Path
import resource
import statistics
import subprocess
import sys
import tempfile
import time
import zipfile

from qualify import SOURCES


def corpus(path):
    rows = {
        r["id"]: r
        for r in json.loads((SOURCES / "classes.json").read_text())["fixtures"]
    }
    members = {
        "Sample.class": "java-8",
        "META-INF/versions/11/Sample.class": "java-11",
        "META-INF/versions/21/Sample.class": "java-21",
        "Features.class": "lambda-generics",
        "Point.class": "record",
        "Effects.class": "effects",
        "example/Greeting.class": "kotlin-1.9.24",
        "Root.class": "hierarchy-root",
        "Middle.class": "hierarchy-middle",
        "Leaf.class": "hierarchy-leaf",
    }
    with zipfile.ZipFile(path, "w") as jar:

        def store(name, data):
            jar.writestr(zipfile.ZipInfo(name), data)

        store(
            "META-INF/MANIFEST.MF",
            b"Manifest-Version: 1.0\r\nMulti-Release: true\r\n\r\n",
        )
        for name, key in members.items():
            data = base64.b64decode(rows[key]["bytes"], validate=True)
            assert hashlib.sha256(data).hexdigest() == rows[key]["sha256"]
            store(name, data)


def measure(library, jar, mode):
    sdk = ctypes.CDLL(str(library.resolve()))
    api = sdk.neverd_jvm_recover_json_v1
    api.restype = ctypes.c_void_p
    api.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p]
    sdk.neverd_free_string.argtypes = [ctypes.c_void_p]
    sdk.neverd_free_string.restype = None
    path = bytes(jar)
    measurements = {}

    def call(label, **options):
        options = json.dumps({"release": 21, **options}).encode()
        t = time.perf_counter_ns()
        p = api(path, options, None, None)
        assert p
        try:
            raw = ctypes.string_at(p)
        finally:
            sdk.neverd_free_string(p)
        elapsed = (time.perf_counter_ns() - t) / 1e6
        result = json.loads(raw)
        assert result["status"] in {
            "reconstructed_source",
            "partial_reconstruction",
            "metadata_only",
        }
        measurements[label] = {
            "ms": elapsed,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "methods": len(result["functions"]),
        }

    selected = {"class": "Sample", "method": "arithmetic(II)I"}
    if mode == "inventory":
        call("cold_inventory", inventory=True)
        call("first_method_after_inventory", **selected)
        call("repeated_method", **selected)
    elif mode == "method":
        call("cold_method", **selected)
    else:
        call("whole_jar")
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    measurements["peak_mib"] = peak / (1024**2 if sys.platform == "darwin" else 1024)
    return measurements


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", required=True, type=Path)
    p.add_argument("--rounds", type=int, default=9)
    p.add_argument("--jar", type=Path)
    p.add_argument("--mode", choices=["inventory", "method", "whole"])
    a = p.parse_args()
    if a.mode:
        print(json.dumps(measure(a.library, a.jar, a.mode)))
        return
    with tempfile.TemporaryDirectory() as tmp:
        jar = Path(tmp) / "qualified.jar"
        corpus(jar)
        metrics = {}
        hashes = {}
        counts = {}
        for _ in range(a.rounds):
            for mode in ("inventory", "method", "whole"):
                data = json.loads(
                    subprocess.check_output(
                        [
                            sys.executable,
                            __file__,
                            "--library",
                            a.library,
                            "--jar",
                            jar,
                            "--mode",
                            mode,
                        ]
                    )
                )
                metrics.setdefault("peak_mib_" + mode, []).append(data.pop("peak_mib"))
                for label, row in data.items():
                    metrics.setdefault(label, []).append(row["ms"])
                    assert hashes.setdefault(label, row["sha256"]) == row["sha256"]
                    counts[label] = row["methods"]
        assert (
            hashes["cold_method"]
            == hashes["first_method_after_inventory"]
            == hashes["repeated_method"]
        )
        medians = {k: round(statistics.median(v), 3) for k, v in metrics.items()}
        size = jar.stat().st_size
        print(
            json.dumps(
                {
                    "rounds": a.rounds,
                    "bytes": size,
                    "input_sha256": hashlib.sha256(jar.read_bytes()).hexdigest(),
                    "medians": medians,
                    "method_denominators": counts,
                    "output_sha256": hashes,
                    "whole_mib_s": round(
                        size
                        / (1024**2)
                        / (statistics.median(metrics["whole_jar"]) / 1000),
                        3,
                    ),
                },
                indent=2,
            )
        )


if __name__ == "__main__":
    main()
