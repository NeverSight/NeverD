"""Run with python3 -m unittest discover -s tools/neverd-mcp/tests -v."""
import io
import json
import queue
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import MAX_CACHED_RESULTS, MAX_RESULT, PROTOCOL, RpcError, Server, serve
from transport import Backend, MAX_FRAME, read_frame, write_frame


class FakeBackend:
    def __init__(self, attached=False):
        self.attached = attached
        self.calls = []
        self.response = {"type": "response", "status": "ok", "revision": "42", "payload": {"items": []}}

    def query(self, operation, payload, revision=None):
        self.calls.append((operation, payload, revision))
        return self.response


def request(method, params=None, request_id=1):
    value = {"jsonrpc": "2.0", "method": method}
    if request_id is not None:
        value["id"] = request_id
    if params is not None:
        value["params"] = params
    return value


INIT = request("initialize", {"protocolVersion": PROTOCOL, "capabilities": {},
                              "clientInfo": {"name": "test", "version": "1"}})


class McpProtocolTests(unittest.TestCase):
    def setUp(self):
        self.backend = FakeBackend()
        self.server = Server(self.backend)

    def ready(self, server=None):
        server = server or self.server
        self.assertEqual(server.handle(INIT)["protocolVersion"], PROTOCOL)
        server.handle(request("notifications/initialized", request_id=None))

    def test_handshake_and_lifecycle(self):
        with self.assertRaises(RpcError) as error:
            self.server.handle(request("tools/list"))
        self.assertEqual(error.exception.code, -32002)
        self.ready()
        tools = self.server.handle(request("tools/list"))["tools"]
        self.assertTrue(all("operation" not in t for t in tools))
        self.assertTrue(all(t["annotations"]["readOnlyHint"] for t in tools))
        with self.assertRaises(RpcError):
            self.server.handle(INIT)

    def test_address_and_revision_remain_strings(self):
        self.ready()
        result = self.server.handle(request("tools/call", {"name": "read_disassembly", "arguments": {
            "address": "0xffffffffffffffff", "limit": 2, "expected_revision": "18446744073709551615"}}))
        self.assertFalse(result["isError"])
        self.assertEqual(self.backend.calls, [("disasm", {"address": "0xffffffffffffffff", "limit": 2}, "18446744073709551615")])

    def test_validation_rejects_path_changes_and_unbounded_pages(self):
        self.ready()
        cases = [("list_functions", {"limit": 513}), ("list_functions", {"offset": -1}),
                 ("list_functions", {"limit": True}), ("project_metadata", {"path": "/new/input"}),
                 ("read_bytes", {"address": "0x10000000000000000"}),
                 ("read_bytes", {"address": 9007199254740993}),
                 ("read_decompilation", {"address": "0x1", "representation": "shell"}),
                 ("open", {"path": "/new/input"}), ("rename", {"name": "new"})]
        for name, arguments in cases:
            with self.subTest(name=name, arguments=arguments), self.assertRaises(RpcError):
                self.server.handle(request("tools/call", {"name": name, "arguments": arguments}))
        self.assertEqual(self.backend.calls, [])

    def test_gui_resources_and_navigation_require_attachment(self):
        self.ready()
        with self.assertRaises(RpcError):
            self.server.handle(request("resources/read", {"uri": "neverd://gui/selection"}))
        attached = Server(FakeBackend(attached=True))
        self.ready(attached)
        result = attached.handle(request("tools/call", {"name": "navigate_gui", "arguments": {"address": "0x123"}}))
        self.assertFalse(result["isError"])
        self.assertEqual(attached.backend.calls[0][0], "navigate")
        with self.assertRaises(RpcError):
            attached.handle(request("resources/read", {"uri": "neverd://project/navigate?address=0x1"}))

    def test_resources_are_scoped_and_paginated(self):
        self.ready()
        self.server.handle(request("resources/read", {"uri": "neverd://project/functions?offset=20&limit=10&expected_revision=42"}))
        self.assertEqual(self.backend.calls[-1], ("functions", {"offset": 20, "limit": 10}, "42"))
        for uri in ("file:///etc/passwd", "https://example.com/", "neverd://other/metadata", "neverd://project/functions?limit=1&limit=2"):
            with self.subTest(uri=uri), self.assertRaises(RpcError):
                self.server.handle(request("resources/read", {"uri": uri}))

    def test_errors_and_budget_are_not_successful_empty_results(self):
        self.ready()
        self.backend.response = {"status": "error", "revision": "42", "error": {"code": "stale_revision"}}
        result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
        self.assertTrue(result["isError"])
        self.backend.response = {"status": "ok", "revision": "42", "payload": {"text": "x" * MAX_CACHED_RESULTS}}
        result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
        self.assertTrue(result["isError"])
        self.assertEqual(result["structuredContent"]["status"], "budget_exceeded")

    def test_large_results_return_bounded_revision_stable_resource_pages(self):
        self.ready()
        expected = {"status": "ok", "revision": "42", "project_id": "project-1", "payload": {"text": "中文\n" * MAX_RESULT}}
        self.backend.response = expected
        result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
        self.assertFalse(result["isError"])
        self.assertEqual(result["content"][0]["type"], "resource_link")
        uri = result["content"][0]["uri"]
        chunks = []
        offset = 0
        self.backend.response = {"status": "ok", "revision": "43"}
        while True:
            response = self.server.handle(request("resources/read", {"uri": uri + f"?offset={offset}&limit=65536&expected_revision=42"}))
            page = json.loads(response["contents"][0]["text"])
            self.assertEqual(page["revision"], "42")
            chunks.append(page["text"])
            if page["complete"]:
                break
            offset = page["next_offset"]
        self.assertEqual(json.loads("".join(chunks)), expected)
        self.assertEqual(len(self.backend.calls), 1)
        with self.assertRaises(RpcError):
            self.server.handle(request("resources/read", {"uri": uri + "?expected_revision=43"}))

    def test_snapshot_cache_eviction_is_explicit(self):
        self.ready()
        self.backend.response = {"status": "ok", "revision": "42", "payload": {"text": "x" * MAX_RESULT}}
        first = None
        for _ in range(5):
            result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
            first = first or result["structuredContent"]["resource_uri"]
        self.assertEqual(len(self.server.result_resources), 4)
        self.assertLessEqual(self.server.cached_bytes, MAX_CACHED_RESULTS)
        with self.assertRaises(RpcError):
            self.server.handle(request("resources/read", {"uri": first}))

    def test_standard_stdio_not_worker_framing(self):
        source = io.BytesIO((json.dumps(INIT) + "\n" + json.dumps(request("notifications/initialized", request_id=None)) + "\n").encode())
        output = io.StringIO()
        serve(self.server, source, output)
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        response = json.loads(lines[0])
        self.assertEqual(response["id"], 1)
        self.assertEqual(response["result"]["protocolVersion"], PROTOCOL)

    def test_malformed_json_and_batch_report_protocol_errors(self):
        output = io.StringIO()
        serve(self.server, io.BytesIO(b"{bad}\n[]\n{}\n"), output)
        errors = [json.loads(line)["error"]["code"] for line in output.getvalue().splitlines()]
        self.assertEqual(errors, [-32700, -32600, -32600])

    def test_worker_framing_partial_reads_and_size_limit(self):
        class Partial(io.BytesIO):
            def read(self, size=-1):
                return super().read(min(size, 2))
        output = io.BytesIO()
        expected = {"address": "0xffffffffffffffff", "text": "中文\nsecond line"}
        write_frame(output, expected)
        self.assertEqual(read_frame(Partial(output.getvalue())), expected)
        for size in (0, MAX_FRAME + 1):
            with self.assertRaises(ValueError):
                read_frame(io.BytesIO(struct.pack(">I", size)))
        with self.assertRaises(EOFError):
            read_frame(io.BytesIO(b"\0\0\0\x10{}"))

    def test_idle_heartbeats_do_not_fill_response_queue(self):
        frames = io.BytesIO()
        for _ in range(100):
            write_frame(frames, {"type": "heartbeat", "revision": "1"})
        write_frame(frames, {"type": "response", "request_id": "next", "status": "ok"})
        backend = Backend.__new__(Backend)
        backend.closed = False
        backend.output = io.BytesIO(frames.getvalue())
        backend.messages = queue.Queue(maxsize=32)
        backend._read()
        self.assertEqual(backend.messages.get_nowait()["request_id"], "next")
        self.assertIsInstance(backend.messages.get_nowait(), EOFError)


if __name__ == "__main__":
    unittest.main()
