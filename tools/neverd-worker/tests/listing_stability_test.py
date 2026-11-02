#!/usr/bin/env python3
"""Optional native-input regression for listing refresh/prefetch stability.

Use an owned native image with distant function chunks. The input and an
address near the affected chunk boundary are supplied locally, never bundled.
"""
import os
from pathlib import Path
import shutil
import sys
import tempfile
from transport_test import Client


def run(worker):
    source = os.environ.get("NEVERD_LISTING_STABILITY_INPUT")
    target = os.environ.get("NEVERD_LISTING_STABILITY_ADDRESS")
    if not source or not target:
        print("Set NEVERD_LISTING_STABILITY_INPUT and NEVERD_LISTING_STABILITY_ADDRESS")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-listing-stability-") as directory:
        path = Path(directory) / Path(source).name
        shutil.copyfile(source, path)
        pdb = os.environ.get("NEVERD_LISTING_STABILITY_PDB")
        if pdb:
            shutil.copyfile(pdb, Path(directory) / Path(pdb).name)
        with Client(worker) as client:
            opened = client.call("open", {"path": str(path), "analysis": False, "warm": False})
            assert opened["status"] == "ok", opened
            def page(address, sub, before, after):
                reply = client.call("listing", {"address": address, "sub": sub,
                                                "before": before, "after": after})
                assert reply["status"] == "ok", reply
                return reply["payload"]
            initial = page(target, 0, 48, 240)
            assert initial["anchor"] >= 9, initial
            top = initial["lines"][initial["anchor"] - 9]
            generations, counts = set(), set()
            for cycle in range(5):
                refreshed = page(top["item"], top["sub"], 48, 240)
                count = client.call("metadata")["payload"]["function_count"]
                first = refreshed["lines"][0]
                prefetched = page(first["item"], first["sub"], 240, 0)
                other_count = client.call("metadata")["payload"]["function_count"]
                # Allow the first pass to warm caches, then require reads to
                # retain both the inventory and the listing's generation.
                if cycle:
                    generations.update((refreshed["generation"], prefetched["generation"]))
                    counts.update((count, other_count))
                    assert len(counts) == 1, sorted(counts)
                    assert len(generations) == 1, sorted(generations)
            print("Repeated native listing refresh/prefetch retained inventory and generation")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
