#!/usr/bin/env python3
"""Black-box protocol, cancellation, resource, and sidecar tests against fake C ABI."""
import json
import os
from contextlib import ExitStack
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time

BASE = "0xffff800012340000"


class Client:
    def __init__(self, executable):
        self.process = subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.backlog = []
        self.logs = bytearray()
        self.counter = 0
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.stderr = threading.Thread(target=self._logs, daemon=True)
        self.stderr.start()
        self.hello = self.next(lambda message: message.get("type") == "hello")
        assert self.hello["protocol_major"] == 1

    def _logs(self):
        while data := self.process.stderr.read(4096):
            self.logs.extend(data)

    def _read(self):
        def exact(size):
            data = b""
            while len(data) < size:
                chunk = self.process.stdout.read(size - len(data))
                if not chunk:
                    raise EOFError("worker stdout closed")
                data += chunk
            return data
        try:
            while True:
                size = struct.unpack(">I", exact(4))[0]
                assert 0 < size <= 8 * 1024 * 1024, f"stdout polluted, length={size}"
                self.messages.put(json.loads(exact(size)))
        except BaseException as error:
            self.messages.put(error)

    def next(self, predicate, timeout=8):
        for index, message in enumerate(self.backlog):
            if predicate(message):
                return self.backlog.pop(index)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty as error:
                raise TimeoutError("response timeout") from error
            if isinstance(message, BaseException):
                raise message
            if predicate(message):
                return message
            self.backlog.append(message)
        raise TimeoutError("response timeout")

    def send(self, operation, payload=None, *, revision=None, fragmented=False, request_id=None):
        self.counter += 1
        request_id = request_id or str(self.counter)
        value = dict(protocol_major=1, request_id=request_id, operation=operation, payload=payload or {})
        if revision is not None:
            value["expected_revision"] = revision
        data = json.dumps(value, ensure_ascii=False).encode()
        framed = struct.pack(">I", len(data)) + data
        if fragmented:
            for chunk in (framed[:1], framed[1:3], framed[3:7], framed[7:]):
                self.process.stdin.write(chunk)
                self.process.stdin.flush()
        else:
            self.process.stdin.write(framed)
            self.process.stdin.flush()
        return request_id

    def response(self, request_id):
        return self.next(lambda message: message.get("type") == "response" and message.get("request_id") == request_id)

    def call(self, operation, payload=None, **kwargs):
        request_id = self.send(operation, payload, **kwargs)
        try:
            return self.response(request_id)
        except TimeoutError as error:
            raise TimeoutError(
                f"worker response timeout: operation={operation!r}, "
                f"request_id={request_id!r}, worker_returncode={self.process.poll()!r}"
            ) from error

    def close(self):
        if self.process.poll() is None:
            self.call("shutdown")
            self.process.stdin.close()
            self.process.wait(timeout=8)
        self.stderr.join(timeout=2)

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        if exception_type is None:
            self.close()
        else:
            # Preserve the assertion and release the writer lock before the
            # temporary directory is removed, including on Windows.
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=8)
            self.stderr.join(timeout=2)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-worker-test-") as directory, ExitStack() as clients:
        binary = Path(directory) / "fixture.bin"
        binary.write_bytes(b"mock input")
        client = clients.enter_context(Client(executable))
        assert client.call("metadata")["error"]["code"] == "not_loaded"
        opened = client.call("open", {"path": str(binary)}, fragmented=True)
        assert opened["status"] == "ok", opened
        revision = opened["revision"]
        assert opened["payload"]["entry_address"] == BASE
        assert opened["payload"]["function_count"] == 600
        functions = client.call("functions", {"limit": 3})["payload"]
        assert len(functions["items"]) == 3 and functions["next_offset"] == 3 and not functions["complete"]
        assert functions["items"][0]["address"] == BASE
        assert client.call("functions", {"offset": 598, "limit": 3})["payload"]["complete"]
        assert client.call("functions", {"filter": "function_599"})["payload"]["total"] == 1
        assert client.call("functions", {"filter": "absent"})["payload"]["items"] == []
        assert client.call("functions", {"limit": 0})["error"]["code"] == "invalid_request"
        assert client.call("functions", {"limit": 513})["status"] == "budget_exceeded"
        assert client.call("metadata", revision="0")["error"]["code"] == "stale_revision"
        assert client.call("bytes", {"address": 123})["error"]["code"] == "invalid_request"
        assert client.call("bytes", {"address": "0xffffffffffffffff", "size": 2})["error"]["code"] == "invalid_address"
        assert client.call("bytes", {"address": BASE, "size": 4})["payload"]["data"] == "00010203"
        disasm = client.call("disasm", {"address": BASE, "limit": 2})["payload"]
        assert disasm["next_address"] == "0xffff800012340002"
        assert disasm["items"][0]["mnemonic"] == "nop"
        assert client.call("resolve", {"query": "function_0"})["payload"]["address"] == BASE
        assert client.call("resolve", {"query": "0xffff800012340003"})["payload"]["function_address"] == BASE
        assert client.call("strings", {"offset": 599, "limit": 4})["payload"]["complete"]

        competitor = clients.enter_context(Client(executable))
        assert competitor.call("open", {"path": str(binary)})["error"]["code"] == "project_locked"
        assert competitor.call("open", {"path": str(binary), "read_only": True})["status"] == "ok"
        assert competitor.call("annotation_set", {"address": BASE, "text": "forbidden"})["error"]["code"] == "read_only"
        competitor.close()

        note = "中文 تعليق 日本語 test"
        edit = client.call("annotation_set", {"address": BASE, "text": note}, revision=revision)
        assert edit["payload"]["dirty"] and not edit["payload"]["saved"]
        assert not Path(str(binary) + ".neverd-annotations.json").exists()
        assert client.call("save")["payload"]["saved"]
        saved = json.loads(Path(str(binary) + ".neverd-annotations.json").read_text(encoding="utf-8"))
        assert saved == [dict(addr=BASE, text=note)]
        renamed = client.call("rename", {"address": BASE, "name": "renamed_function"})
        assert renamed["payload"]["saved"]
        assert client.call("functions", {"filter": "renamed_function"})["payload"]["total"] == 1
        client.call("annotation_set", {"address": BASE, "text": "temporary"})
        client.call("reload")
        assert client.call("annotations")["payload"]["items"][0]["text"] == note

        # While synchronous engine work runs, framing, queue admission,
        # heartbeat and cancellation continue on the transport thread.
        analyze_id = client.send("analyze")
        heartbeat = client.next(lambda item: item.get("type") == "heartbeat" and item.get("active_request_id") == analyze_id)
        assert heartbeat["revision"] != "0"
        pending = [client.send("metadata", request_id=f"queued-{index}") for index in range(40)]
        cancel = client.call("cancel", {"request_id": analyze_id})["payload"]
        assert cancel["accepted"] and not cancel["stopped"] and cancel["requires_restart"]
        queued_cancel = client.call("cancel", {"request_id": pending[0]})["payload"]
        assert queued_cancel["accepted"] and queued_cancel["stopped"]
        responses = [client.response(request_id) for request_id in pending]
        assert any(item.get("error", {}).get("code") == "queue_full" for item in responses)
        assert responses[0]["status"] == "cancelled"
        analysis = client.response(analyze_id)
        assert analysis["status"] == "ok" and analysis["cancellation_requested"] and analysis["calculation_stopped"]
        first = client.call("decompile", {"address": BASE, "representation": "llvm", "limit": 400})["payload"]
        last = client.call("decompile", {"address": BASE, "representation": "llvm", "offset": first["next_offset"], "limit": 400})["payload"]
        assert first["total_lines"] == 700 and not first["complete"] and last["complete"]
        assert "code line 400" in last["text"]
        assert first["mapping_status"] == "unsupported_representation" and first["rows"] == []
        for stage in ("low", "med"):
            mapped = client.call("decompile", {"address": BASE, "representation": stage, "offset": 0, "limit": 3})
            assert mapped["status"] == "ok", mapped
            page = mapped["payload"]
            assert page["mapping_status"] == "instruction_anchors", page
            assert page["revision"] == mapped["revision"] and page["project_id"] == mapped["project_id"]
            assert [row["line"] for row in page["rows"]] == [0, 1, 2]
            assert page["rows"][0]["addresses"] == []
            assert page["rows"][1]["addresses"] == [hex(int(BASE, 16) + 1)]
            second = client.call("decompile", {"address": BASE, "representation": stage, "offset": 3, "limit": 3})["payload"]
            assert second["rows"][0]["line"] == 3 and "code line 3" in second["text"]
        cfg = client.call("cfg", {"address": BASE})["payload"]
        assert isinstance(cfg["nodes"][0]["id"], str) and cfg["complete"]
        assert client.call("cfg", {"address": "0xffff800012340001"})["status"] == "budget_exceeded"
        assert client.call("xrefs", {"address": BASE})["payload"]["items"][0]["address"] == "0xffff800012340008"
        client.close()
        assert b"native stdout" in client.logs and b"python-style print" in client.logs
        assert client.process.returncode == 0

        # OS releases the writer lock on exit, and acknowledged edits survive.
        reopened = clients.enter_context(Client(executable))
        assert reopened.call("open", {"path": str(binary)})["status"] == "ok"
        assert reopened.call("annotations")["payload"]["items"][0]["text"] == note
        assert reopened.call("functions", {"filter": "renamed_function"})["payload"]["total"] == 1
        reopened.close()

    malformed = Client(executable)
    malformed.process.stdin.write(struct.pack(">I", 1) + b"{")
    malformed.process.stdin.flush()
    assert malformed.next(lambda m: m.get("type") == "response")["error"]["code"] == "invalid_json"
    assert malformed.call("hello")["status"] == "ok"
    malformed.process.stdin.write(struct.pack(">I", 8 * 1024 * 1024 + 1))
    malformed.process.stdin.flush()
    assert malformed.next(lambda m: m.get("type") == "fatal")["error"]["code"] == "invalid_frame"
    malformed.process.wait(timeout=3)
    assert malformed.process.returncode == 2
    print("worker transport, lifecycle, paging, bounds, cancellation, precision, and persistence passed")


if __name__ == "__main__":
    run(sys.argv[1])
