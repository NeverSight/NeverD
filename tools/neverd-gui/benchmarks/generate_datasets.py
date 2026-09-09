#!/usr/bin/env python3
"""Generate compact deterministic benchmark descriptors, not giant resident arrays."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    evm = b"0x600160020100"
    (args.output / "arithmetic.evm").write_bytes(evm)
    source = "__attribute__((noinline)) int gui_add(int a, int b) { return a + b; }\nint main(void) { return gui_add(2, 3); }\n"
    (args.output / "native_sample.c").write_text(source, encoding="utf-8")
    manifest = {
        "schema": 1,
        "generator": "neverd-gui-synthetic-v1",
        "seed": 3389,
        "synthetic_function_counts": [100000, 1000000],
        "synthetic_text_rows": 10000000,
        "synthetic_graph_nodes": [1000, 10000],
        "row_address": "0xffff800000000000 + uint64(row) * 16; formatted in C++",
        "row_window": 512,
        "graph": "deterministic grid with two outgoing edges, analytical viewport culling",
        "real_fixture": {"path": "arithmetic.evm", "sha256": hashlib.sha256(evm).hexdigest(), "purpose": "benign arithmetic analysis, no execution"},
        "native_fixture": {"path": "native_sample.c", "sha256": hashlib.sha256(source.encode()).hexdigest()},
        "distinction": "Synthetic UI cardinalities are not functions recovered by the engine from the tiny real fixture.",
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(args.output / "manifest.json")


if __name__ == "__main__":
    main()
