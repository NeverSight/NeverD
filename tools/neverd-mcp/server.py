#!/usr/bin/env python3
"""NeverD read-only MCP adapter, protocol 2025-11-25, standard newline stdio."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import OrderedDict
import json
import re
import signal
import sys
import threading
from urllib.parse import parse_qs, urlparse

from transport import Backend

PROTOCOL = "2025-11-25"
MAX_LINE = 1024 * 1024
MAX_RESULT = 512 * 1024
MAX_CACHED_RESULTS = 8 * 1024 * 1024
ADDRESS = {"type": "string", "pattern": "^0x[0-9a-fA-F]{1,16}$"}
PAGE = {"offset": {"type": "integer", "minimum": 0, "maximum": 2147483647},
        "limit": {"type": "integer", "minimum": 1, "maximum": 512}}
REVISION = {"type": "string", "maxLength": 128}


def tool(name, operation, description, properties=None, required=()):
    return {"name": name, "description": description, "operation": operation,
            "inputSchema": {"type": "object", "properties": {
                **(properties or {}), "expected_revision": REVISION},
                "required": list(required), "additionalProperties": False},
            "annotations": {"readOnlyHint": True, "destructiveHint": False,
                            "idempotentHint": True, "openWorldHint": False}}


TOOLS = [
    tool("project_metadata", "metadata", "Read metadata for the explicitly opened NeverD input."),
    tool("list_functions", "functions", "Read a bounded function page; preserve revision across pages.",
         {**PAGE, "filter": {"type": "string", "maxLength": 256}}),
    tool("read_disassembly", "disasm", "Read an instruction window at a virtual address.",
         {"address": ADDRESS, "limit": PAGE["limit"]}, ("address",)),
    tool("read_decompilation", "decompile", "Read C or IR for one function. May require analysis.",
         {**PAGE, "address": ADDRESS, "representation": {"type": "string", "enum": ["c", "low", "med", "high", "llvm"]}}, ("address",)),
    tool("read_bytes", "bytes", "Read up to 4096 mapped bytes at a virtual address.",
         {"address": ADDRESS, "size": {"type": "integer", "minimum": 1, "maximum": 4096}}, ("address",)),
    tool("list_xrefs", "xrefs", "Read references associated with a virtual address.",
         {**PAGE, "address": ADDRESS, "direction": {"type": "string", "enum": ["to", "from"]}}, ("address",)),
    tool("list_strings", "strings", "Read a bounded string page.", {**PAGE}),
    tool("read_cfg", "cfg", "Read the control-flow graph for one function.", {"address": ADDRESS}, ("address",)),
]
SELECTION = tool("current_selection", "selection", "Read the current selection in the attached GUI session.")
NAVIGATION = [tool("navigate_gui", "navigate", "Navigate the attached GUI to an explicit virtual address.", {"address": ADDRESS}, ("address",)),
              tool("highlight_gui", "highlight", "Highlight an explicit address in the attached GUI.", {"address": ADDRESS}, ("address",))]
for _navigation in NAVIGATION:
    _navigation["annotations"]["readOnlyHint"] = False


class RpcError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


def validate(arguments, schema):
    if not isinstance(arguments, dict):
        raise RpcError(-32602, "arguments must be an object")
    properties = schema["properties"]
    if set(arguments) - set(properties) or set(schema["required"]) - set(arguments):
        raise RpcError(-32602, "Unknown or missing tool argument")
    for key, value in arguments.items():
        rule = properties[key]
        if rule["type"] == "integer":
            valid = type(value) is int and rule["minimum"] <= value <= rule["maximum"]
        else:
            valid = isinstance(value, str) and len(value) <= rule.get("maxLength", 4096)
            if "enum" in rule:
                valid = valid and value in rule["enum"]
            if "pattern" in rule:
                valid = valid and re.fullmatch(rule["pattern"], value) is not None
        if not valid:
            raise RpcError(-32602, "Invalid argument: " + key)


class Server:
    def __init__(self, backend):
        self.backend = backend
        self.initialized = False
        self.ready = False
        self.tools = TOOLS + ([SELECTION, *NAVIGATION] if backend.attached else [])
        self.result_resources = OrderedDict()
        self.cached_bytes = 0
        self.result_serial = 0

    def handle(self, request):
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            raise RpcError(-32600, "Invalid JSON-RPC request")
        if "id" in request and (type(request["id"]) not in (str, int)):
            raise RpcError(-32600, "Invalid request id")
        method = request["method"]
        params = request.get("params", {})
        if not isinstance(params, dict):
            raise RpcError(-32602, "params must be an object")
        if method == "initialize":
            if self.initialized:
                raise RpcError(-32600, "Session already initialized")
            if not isinstance(params.get("protocolVersion"), str) or not isinstance(params.get("capabilities"), dict) or not isinstance(params.get("clientInfo"), dict):
                raise RpcError(-32602, "Missing initialization parameters")
            self.initialized = True
            return {"protocolVersion": PROTOCOL, "capabilities": {"tools": {}, "resources": {}},
                    "serverInfo": {"name": "neverd-mcp", "version": "0.1.0"}}
        if method == "ping":
            return {}
        if method == "notifications/initialized":
            if self.initialized:
                self.ready = True
            return None
        if not self.ready:
            raise RpcError(-32002, "Initialize the MCP session first")
        if method == "tools/list":
            if params.get("cursor"):
                raise RpcError(-32602, "Unknown tool cursor")
            return {"tools": [{k: v for k, v in item.items() if k != "operation"} for item in self.tools]}
        if method == "tools/call":
            selected = next((t for t in self.tools if t["name"] == params.get("name")), None)
            if selected is None:
                raise RpcError(-32602, "Unknown tool")
            arguments = params.get("arguments", {})
            validate(arguments, selected["inputSchema"])
            arguments = dict(arguments)
            revision = arguments.pop("expected_revision", None)
            try:
                result = self.backend.query(selected["operation"], arguments, revision)
                return self.tool_result(result)
            except (OSError, EOFError, ValueError, RuntimeError, TimeoutError) as error:
                return {"content": [{"type": "text", "text": str(error)}], "isError": True}
        if method == "resources/list":
            if params.get("cursor"):
                raise RpcError(-32602, "Unknown resource cursor")
            resources = [{"uri": "neverd://project/metadata", "name": "Project metadata", "mimeType": "application/json"},
                         {"uri": "neverd://project/functions", "name": "Functions (first page)", "mimeType": "application/json"}]
            if self.backend.attached:
                resources.append({"uri": "neverd://gui/selection", "name": "GUI selection", "mimeType": "application/json"})
            for uri in self.result_resources:
                resources.append({"uri": uri, "name": "Result snapshot " + uri.rsplit("/", 1)[-1], "mimeType": "application/json"})
            return {"resources": resources}
        if method == "resources/templates/list":
            return {"resourceTemplates": [
                {"uriTemplate": "neverd://project/functions{?offset,limit,filter,expected_revision}", "name": "Function page", "mimeType": "application/json"},
                {"uriTemplate": "neverd://project/disasm{?address,limit,expected_revision}", "name": "Disassembly", "mimeType": "application/json"},
                {"uriTemplate": "neverd://project/decompile{?address,representation,offset,limit,expected_revision}", "name": "C and IR", "mimeType": "application/json"},
                {"uriTemplate": "neverd://project/xrefs{?address,direction,offset,limit,expected_revision}", "name": "Cross references", "mimeType": "application/json"}]}
        if method == "resources/read":
            return self.read_resource(params.get("uri"))
        if method.startswith("notifications/"):
            return None
        raise RpcError(-32601, "Method not found")

    def tool_result(self, result):
        encoded = json.dumps(result, ensure_ascii=False, separators=(",", ":"))
        size = len(encoded.encode("utf-8"))
        if MAX_RESULT < size <= MAX_CACHED_RESULTS:
            while self.result_resources and (len(self.result_resources) >= 4 or self.cached_bytes + size > MAX_CACHED_RESULTS):
                _, expired = self.result_resources.popitem(last=False)
                self.cached_bytes -= expired["bytes"]
            self.result_serial += 1
            uri = "neverd://result/" + str(self.result_serial)
            self.result_resources[uri] = {"text": encoded, "bytes": size,
                                          "revision": result.get("revision"), "project_id": result.get("project_id")}
            self.cached_bytes += size
            reference = {"status": result.get("status"), "project_id": result.get("project_id"),
                         "revision": result.get("revision"), "resource_uri": uri,
                         "total_characters": len(encoded), "inline_complete": False}
            return {"content": [{"type": "resource_link", "uri": uri, "name": "NeverD result snapshot",
                                 "mimeType": "application/json", "description": "Read this snapshot in character pages using offset and limit URI parameters."},
                                {"type": "text", "text": json.dumps(reference)}],
                    "structuredContent": reference, "isError": result.get("status") not in ("ok", "success")}
        if size > MAX_CACHED_RESULTS:
            result = {"status": "budget_exceeded", "revision": result.get("revision"),
                      "error": {"message": "Result exceeds MCP 8 MiB snapshot budget; request a smaller page/window."}}
            encoded = json.dumps(result)
        return {"content": [{"type": "text", "text": encoded}], "structuredContent": result,
                "isError": result.get("status") not in ("ok", "success")}

    def read_resource(self, uri):
        if not isinstance(uri, str):
            raise RpcError(-32602, "uri must be a string")
        parsed = urlparse(uri)
        if parsed.scheme != "neverd" or parsed.fragment or parsed.username or parsed.port:
            raise RpcError(-32602, "Unsupported resource URI")
        if parsed.netloc == "result":
            return self.read_result_resource(uri, parsed)
        operation = parsed.path.lstrip("/")
        if parsed.netloc == "gui" and operation == "selection" and self.backend.attached:
            selected = SELECTION
        elif parsed.netloc == "project":
            selected = next((t for t in TOOLS if t["operation"] == operation), None)
        else:
            selected = None
        if selected is None:
            raise RpcError(-32002, "Resource not found")
        arguments = {}
        for key, values in parse_qs(parsed.query, keep_blank_values=True).items():
            if len(values) != 1:
                raise RpcError(-32602, "Repeated resource parameter")
            value = values[0]
            if key in ("offset", "limit", "size"):
                try:
                    value = int(value)
                except ValueError as error:
                    raise RpcError(-32602, "Invalid numeric resource parameter") from error
            arguments[key] = value
        validate(arguments, selected["inputSchema"])
        revision = arguments.pop("expected_revision", None)
        result = self.tool_result(self.backend.query(operation, arguments, revision))
        if result["isError"]:
            raise RpcError(-32002, result["content"][-1]["text"])
        return {"contents": [{"uri": uri, "mimeType": "application/json", "text": result["content"][-1]["text"]}]}

    def read_result_resource(self, uri, parsed):
        if re.fullmatch(r"/[1-9][0-9]*", parsed.path) is None:
            raise RpcError(-32002, "Result snapshot not found")
        key = "neverd://result" + parsed.path
        record = self.result_resources.get(key)
        if record is None:
            raise RpcError(-32002, "Result snapshot expired or not found; repeat the original query")
        parameters = parse_qs(parsed.query, keep_blank_values=True)
        if set(parameters) - {"offset", "limit", "expected_revision"} or any(len(values) != 1 for values in parameters.values()):
            raise RpcError(-32602, "Invalid result page parameters")
        try:
            offset = int(parameters.get("offset", ["0"])[0])
            limit = int(parameters.get("limit", ["32768"])[0])
        except ValueError as error:
            raise RpcError(-32602, "Invalid result page range") from error
        if offset < 0 or offset > len(record["text"]) or not 1 <= limit <= 65536:
            raise RpcError(-32602, "Result page is outside its bounds")
        expected = parameters.get("expected_revision", [None])[0]
        if expected is not None and expected != record["revision"]:
            raise RpcError(-32602, "Result snapshot revision does not match")
        end = min(offset + limit, len(record["text"]))
        page = {"project_id": record["project_id"], "revision": record["revision"],
                "encoding": "json-text", "offset": offset, "total_characters": len(record["text"]),
                "text": record["text"][offset:end], "next_offset": end if end < len(record["text"]) else None,
                "complete": end == len(record["text"])}
        return {"contents": [{"uri": uri, "mimeType": "application/json", "text": json.dumps(page, ensure_ascii=False)}]}


def serve(server, source, sink):
    lock = threading.Lock()
    pending = {}
    executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="neverd-mcp")

    def emit(response):
        with lock:
            sink.write(json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n")
            sink.flush()

    def respond(request, cancel, slot):
        request_id = request.get("id") if isinstance(request, dict) else None
        if cancel.is_set():
            with lock:
                pending.pop(slot, None)
            return
        try:
            result = server.handle(request)
            response = {"jsonrpc": "2.0", "id": request_id, "result": result}
        except RpcError as error:
            response = {"jsonrpc": "2.0", "id": request_id,
                        "error": {"code": error.code, "message": error.message}}
        except Exception as error:
            response = {"jsonrpc": "2.0", "id": request_id,
                        "error": {"code": -32603, "message": str(error)}}
        invalid_request = response.get("error", {}).get("code") == -32600
        if not cancel.is_set() and (invalid_request or not isinstance(request, dict) or "id" in request):
            emit(response)
        with lock:
            pending.pop(slot, None)

    try:
        while True:
            line = source.readline(MAX_LINE + 1)
            if not line:
                break
            if len(line) > MAX_LINE:
                emit({"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "MCP message exceeds 1 MiB"}})
                break
            try:
                request = json.loads(line)
            except (ValueError, UnicodeError):
                emit({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Invalid JSON"}})
                continue
            if isinstance(request, dict) and request.get("method") == "notifications/cancelled":
                params = request.get("params", {})
                request_id = params.get("requestId") if isinstance(params, dict) else None
                if type(request_id) in (str, int):
                    with lock:
                        cancellation = pending.get(request_id)
                    if cancellation:
                        cancellation.set()
                continue
            request_id = request.get("id") if isinstance(request, dict) else None
            if request_id is not None and type(request_id) not in (str, int):
                emit({"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid request id"}})
                continue
            cancel = threading.Event()
            slot = request_id if request_id is not None else object()
            with lock:
                overloaded = len(pending) >= 16 or (request_id is not None and request_id in pending)
                if not overloaded:
                    pending[slot] = cancel
            if overloaded:
                if request_id is not None:
                    emit({"jsonrpc": "2.0", "id": request_id, "error": {"code": -32000, "message": "Request queue full or duplicate id"}})
                continue
            executor.submit(respond, request, cancel, slot)
    finally:
        executor.shutdown(wait=True)


def main():
    if sys.version_info < (3, 10):
        print("neverd-mcp: Python 3.10+ is required; configure a supported interpreter on PATH", file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", help="Absolute path to the matching neverd-worker executable")
    parser.add_argument("--file", help="Local input, explicitly scoped to this MCP session")
    parser.add_argument("--attach", help="Private credential file exported by an open GUI session")
    args = parser.parse_args()
    if bool(args.attach) == bool(args.worker or args.file) or (not args.attach and not (args.worker and args.file)):
        parser.error("Use either --worker ABSOLUTE_PATH --file INPUT, or --attach CREDENTIAL_FILE")
    backend = None
    try:
        backend = Backend(worker=args.worker, path=args.file, attach=args.attach)
        def terminate(_signal, _frame):
            backend.close()
            raise SystemExit(0)
        signal.signal(signal.SIGTERM, terminate)
        signal.signal(signal.SIGINT, terminate)
        serve(Server(backend), sys.stdin.buffer, sys.stdout)
        return 0
    except (OSError, ValueError, RuntimeError, EOFError) as error:
        print("neverd-mcp: " + str(error), file=sys.stderr)
        return 1
    finally:
        if backend:
            backend.close()


if __name__ == "__main__":
    sys.exit(main())
