#!/usr/bin/env python3
"""Actual framed IPC through Engine with a deterministic 10k-block C ABI fixture."""
import json
from pathlib import Path
import sys
import tempfile
import time
from transport_test import Client, BASE


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-graph-worker-") as directory:
        binary = Path(directory) / "fixture.bin"
        binary.write_bytes(b"benign graph fixture")
        client = Client(executable)
        try:
            assert "cfg_viewport" in client.hello["capabilities"]
            assert client.call("open", {"path": str(binary)})["status"] == "ok"
            address = hex(int(BASE, 16) + 2)
            assert client.call("cfg_viewport", {"address": address})["error"]["code"] == "stale_layout"
            summary = client.call("cfg_summary", {"address": address})
            assert summary["status"] == "ok", summary
            data = summary["payload"]
            assert data["node_count"] == 10000 and data["edge_count"] == 9999
            assert "nodes" not in data and len(json.dumps(summary)) < 2048
            request = dict(address=address, layout_revision=data["layout_revision"], x=0, y=0,
                           width=1100, height=700, scale=1)
            latencies = []
            for i in range(100):
                request["y"] = i * 10000
                begin = time.perf_counter()
                reply = client.call("cfg_viewport", request, revision=summary["revision"])
                latencies.append((time.perf_counter() - begin) * 1000)
                assert reply["status"] == "ok", reply
                result = reply["payload"]
                assert result["snapshot_complete"] and result["complete"]
                assert len(result["nodes"]) <= 4 and len(result["edges"]) <= 4
                assert len(json.dumps(reply)) < 16384
                for node in result["nodes"]:
                    assert isinstance(node["address"], str) and int(node["address"], 16) >= int(address, 16)
            request.update(data["bounds"])
            page = client.call("cfg_viewport", request)["payload"]
            assert len(page["nodes"]) == 256 and len(page["edges"]) == 512
            assert not page["complete"] and page["nodes_truncated"] and page["edges_truncated"]
            # Mutation invalidates the cached geometry even though an annotation
            # does not change topology: stale tiles must not enter a new revision.
            assert client.call("annotation_set", {"address": BASE, "text": "note"})["status"] == "ok"
            assert client.call("cfg_viewport", request)["error"]["code"] == "stale_layout"
            new_summary = client.call("cfg_summary", {"address": address})["payload"]
            assert new_summary["layout_revision"] != data["layout_revision"]
            assert client.call("cfg_viewport", request)["error"]["code"] == "stale_layout"
            assert client.call("cfg", {"address": address})["status"] == "budget_exceeded"
            latencies.sort()
            print(f"10k CFG IPC: 100 local viewport queries, p50={latencies[49]:.3f} ms, p95={latencies[94]:.3f} ms; bounded tiles, stale revision and legacy compatibility passed")
        finally:
            client.close()


if __name__ == "__main__":
    run(sys.argv[1])
