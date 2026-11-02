#!/usr/bin/env python3
"""Discovery admits replica snapshots before rebuilding the browser listing."""
from pathlib import Path
import sys
import tempfile
import time

from transport_test import Client


def ok(client, operation, payload=None, **kwargs):
    reply = client.call(operation, payload, **kwargs)
    assert reply["status"] == "ok", reply
    return reply


def ready(client, opened):
    message = client.next(lambda m: m.get("type") == "heartbeat"
                          and m.get("project_id") == opened["project_id"]
                          and (m.get("background") or {}).get("discovery_complete"))
    assert message["revision"] == opened["revision"], message
    assert message["active_request_id"] is None, message
    assert not message["cancellation_requested"], message
    assert "web" in message, message
    return message["background"]


def wait_for_file(path):
    deadline = time.monotonic() + 8
    while not path.exists():
        assert time.monotonic() < deadline, f"Fixture gate not reached: {path}"
        time.sleep(0.005)


def indexed(client, opened):
    message = client.next(lambda m: m.get("type") == "heartbeat"
                          and m.get("project_id") == opened["project_id"]
                          and (m.get("background") or {}).get("state") == "ready")
    assert message["background"]["functions"] == 601, message


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-discovery-ready-") as directory, Client(executable) as client:
        assert client.call("analysis_snapshot")["error"]["code"] == "not_loaded"
        # Existing clients do not opt in to an extra owned image. Even a
        # stepped-capable SDK must use synchronous discovery for this open.
        binary = Path(directory) / "discovery-sync-only.bin"
        binary.write_bytes(b"fixture")
        refused = client.call("open", {"path": str(binary), "step_discovery": "yes"})
        assert refused["error"]["code"] == "invalid_request", refused
        opened = ok(client, "open", {"path": str(binary), "warm": False})
        state = ready(client, opened)
        assert state["state"] != "unavailable", state
        assert ok(client, "functions", {"limit": 1})["payload"]["total"] == 600
        # Reopening must publish a fresh readiness edge for the new project.
        for sequence in range(2):
            binary = Path(directory) / f"discovery-order-{sequence}.bin"
            binary.write_bytes(b"fixture")
            Path(str(binary) + ".pause-discovery").touch()
            opened = ok(client, "open", {"path": str(binary), "warm": False, "step_discovery": True})
            wait_for_file(Path(str(binary) + ".discovery-waiting"))
            snapshot_id = client.send("analysis_snapshot", revision=opened["revision"])
            # hello is answered by the input loop. Its reply proves the prior
            # snapshot was admitted while the executor still waits in the SDK.
            ok(client, "hello")
            Path(str(binary) + ".finish-discovery").touch()
            state = ready(client, opened)
            assert state["state"] == "pending", state
            # No listing is built yet, so no count is published: a client
            # would read 0 as every function gone and reload its lists.
            assert "functions" not in state, state
            snapshot = client.response(snapshot_id)
            assert snapshot["status"] == "ok", snapshot
            assert snapshot["project_id"] == opened["project_id"], snapshot
            assert Path(snapshot["payload"]["open"]["path"]).resolve() == binary.resolve()
            # The reply records the state when the serialized executor served
            # this request. The worker may start rebuilding before the client
            # gets scheduled again, so a later filesystem read cannot prove
            # which operation ran first.
            assert snapshot["background"]["state"] == "pending", snapshot
            assert "functions" not in snapshot["background"], snapshot
            Path(str(binary) + ".continue").touch()
            indexed(client, opened)
            assert ok(client, "functions", {"limit": 1})["payload"]["total"] == 601

        # The idle grace is bounded. A consumer arriving after indexing must
        # receive the completed state, even if it kept an earlier ready edge.
        binary = Path(directory) / "discovery-order-late.bin"
        binary.write_bytes(b"fixture")
        Path(str(binary) + ".continue").touch()
        opened = ok(client, "open", {"path": str(binary), "warm": False, "step_discovery": True})
        assert ready(client, opened)["state"] == "pending"
        indexed(client, opened)
        snapshot = ok(client, "analysis_snapshot", revision=opened["revision"])
        assert snapshot["background"]["state"] == "ready", snapshot
        assert snapshot["background"]["functions"] == 601, snapshot

        # A later project without the option must not inherit a previous
        # project's permission to retain a discovery snapshot.
        binary = Path(directory) / "discovery-sync-only.bin"
        opened = ok(client, "open", {"path": str(binary), "warm": False})
        assert ready(client, opened)["state"] != "unavailable"
        assert ok(client, "functions", {"limit": 1})["payload"]["total"] == 600

        binary = Path(directory) / "discovery-fail.bin"
        binary.write_bytes(b"fixture")
        opened = ok(client, "open", {"path": str(binary), "warm": False, "step_discovery": True})
        state = ready(client, opened)
        assert state["state"] == "unavailable", state
        assert state["error"] == "Fixture discovery failed", state
        ok(client, "analysis_snapshot", revision=opened["revision"])
        assert ok(client, "functions", {"limit": 1})["payload"]["total"] == 600

        # Shutdown admitted during discovery must not start a listing rebuild
        # or wait for that rebuild's release file before the process can exit.
        binary = Path(directory) / "discovery-order-shutdown.bin"
        binary.write_bytes(b"fixture")
        Path(str(binary) + ".pause-discovery").touch()
        ok(client, "open", {"path": str(binary), "warm": False, "step_discovery": True})
        wait_for_file(Path(str(binary) + ".discovery-waiting"))
        ok(client, "shutdown")
        Path(str(binary) + ".finish-discovery").touch()
        client.process.stdin.close()
        assert client.process.wait(timeout=8) == 0
        assert not Path(str(binary) + ".rebuild").exists()


if __name__ == "__main__":
    run(sys.argv[1])
    print("Discovery notification, snapshot priority, indexing, reload and failure checks passed")
