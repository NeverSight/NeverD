"""Standard-library provider helper for NeverD GUI process protocol v1.

Providers run outside NeverD and do not require the embedded Python plugin SDK.
One process serves one job. stdout is reserved for framed protocol messages.
"""
from __future__ import annotations

import hashlib
from collections import deque
import json
from pathlib import Path
import struct
import sys
from typing import Any, Callable

MAX_FRAME = 1024 * 1024


class QueryError(RuntimeError):
    """A host refusal with its original status/code, not a transport error."""

    def __init__(self, reply: dict[str, Any]):
        self.reply = reply
        self.status = reply.get("status", "error")
        error = reply.get("error", {})
        self.code = error.get("code", self.status) if isinstance(error, dict) else self.status
        message = error.get("message", str(error)) if isinstance(error, dict) else str(error)
        super().__init__(f"{self.code}: {message}")


def _read_exact(stream, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = stream.read(size - len(result))
        if not block:
            raise EOFError("GUI connection closed during a frame")
        result.extend(block)
    return bytes(result)


def read_frame(stream) -> dict[str, Any]:
    size, = struct.unpack(">I", _read_exact(stream, 4))
    if not 0 < size <= MAX_FRAME:
        raise ValueError("Invalid GUI frame size")
    message = json.loads(_read_exact(stream, size).decode("utf-8"))
    if (not isinstance(message, dict) or type(message.get("protocol")) is not int
            or message.get("protocol") != 1):
        raise ValueError("Unsupported GUI protocol")
    return message


def write_frame(stream, message: dict[str, Any]) -> None:
    data = json.dumps(dict(message, protocol=1), ensure_ascii=False,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")
    if not 0 < len(data) <= MAX_FRAME:
        raise ValueError("GUI message exceeds the frame limit")
    stream.write(struct.pack(">I", len(data)) + data)
    stream.flush()


class Task:
    """Captured context and bounded host services for one invocation."""

    def __init__(self, request: dict[str, Any], input_stream=None, output_stream=None):
        if request.get("type") != "start" or not isinstance(request.get("task_id"), str):
            raise ValueError("Expected a GUI start request")
        self.id = request["task_id"]
        self.action = request["action"]
        self.context = request["context"]
        self.parameters = request["parameters"]
        self.output_dir = Path(request["output_dir"])
        self._input = input_stream if input_stream is not None else sys.stdin.buffer
        self._output = output_stream if output_stream is not None else sys.stdout.buffer
        self._query_id = 0
        self.extensions = request.get("extensions", [])
        self._events = deque()
        self._event_handlers: dict[str, Callable[[dict[str, Any]], None]] = {}
        self._in_event_handler = False

    def send(self, message_type: str, **payload: Any) -> None:
        write_frame(self._output, dict(type=message_type, task_id=self.id, **payload))

    def log(self, text: str) -> None:
        self.send("log", text=text)

    def progress(self, label: str, done: int | None = None,
                 total: int | None = None) -> None:
        progress: dict[str, Any] = dict(label=label)
        if done is not None:
            progress["done"] = done
        if total is not None:
            progress["total"] = total
        self.send("progress", progress=progress)

    def query(self, operation: str, **payload: Any) -> dict[str, Any]:
        result = self._request("query", "query_reply", operation, payload)
        return result.get("payload", {})

    def _event(self, message: dict[str, Any]) -> None:
        if ("host_events" not in self.extensions or message.get("task_id") != self.id
                or not isinstance(message.get("subscription"), str)
                or not isinstance(message.get("event"), dict)):
            raise ValueError("Invalid host event notification")
        handler = self._event_handlers.get(message["subscription"])
        if handler is None:
            self._events.append(message)
        else:
            self._in_event_handler = True
            try:
                handler(message["event"])
            finally:
                self._in_event_handler = False

    def _read_reply(self, message_type: str, request_id: str) -> dict[str, Any]:
        while True:
            reply = read_frame(self._input)
            if reply.get("type") == "event":
                self._event(reply)
                continue
            if (reply.get("type") != message_type or reply.get("task_id") != self.id
                    or reply.get("id") != request_id):
                raise ValueError("Mismatched host reply")
            result = reply["reply"]
            if result.get("status") != "ok":
                raise QueryError(result)
            return result

    def _request(self, kind: str, reply_type: str, operation: str,
                 payload: dict[str, Any]) -> dict[str, Any]:
        if self._in_event_handler:
            raise RuntimeError("Issue host requests after returning from the event handler")
        self._query_id += 1
        request_id = str(self._query_id)
        self.send(kind, id=request_id, operation=operation, payload=payload)
        return self._read_reply(reply_type, request_id)

    def _host(self, operation: str, **payload: Any) -> dict[str, Any]:
        if "host_services" not in self.extensions:
            raise RuntimeError("This host does not offer service/event extensions")
        return self._request("host", "host_reply", operation, payload)["payload"]

    def services(self) -> dict[str, Any]:
        """Captured service descriptors and missing declared versions."""
        return self._host("services.list")

    def events(self) -> dict[str, Any]:
        return self._host("events.list")

    @staticmethod
    def _binding(catalog: dict[str, Any], family: str, name: str, version: int) -> dict[str, Any]:
        matches = [item for item in catalog[family] if item["id"] == name and item["version"] == version]
        if len(matches) != 1:
            raise QueryError(dict(status="error", error=dict(
                code="capability_ambiguous" if matches else "capability_unavailable",
                message=f"Expected one captured binding for {name}@{version}")))
        item = matches[0]
        if not item["declared"]:
            raise QueryError(dict(status="error", error=dict(
                code="capability_undeclared", message=f"Declare {name}@{version} in the manifest")))
        return item

    def service(self, name: str, version: int = 1, **payload: Any) -> Any:
        binding = self._binding(self.services(), "services", name, version)
        return self._host("services.call", registration=binding["registration"], payload=payload)

    def subscribe(self, name: str, version: int = 1,
                  handler: Callable[[dict[str, Any]], None] | None = None) -> str:
        """Delivery occurs while reading replies or pumping next_event().

        A handler must not issue nested requests; return to the provider loop
        before making another call. No background Python thread is introduced.
        """
        binding = self._binding(self.events(), "events", name, version)
        subscription = self._host("events.subscribe", registration=binding["registration"])["subscription"]
        if handler is not None:
            self._event_handlers[subscription] = handler
        return subscription

    def unsubscribe(self, subscription: str) -> None:
        self._host("events.unsubscribe", subscription=subscription)
        self._event_handlers.pop(subscription, None)
        self._events = deque(item for item in self._events if item["subscription"] != subscription)

    def next_event(self) -> dict[str, Any]:
        """Wait for one queued notification; the action's host deadline applies."""
        if self._events:
            return self._events.popleft()
        message = read_frame(self._input)
        if message.get("type") != "event":
            raise ValueError("Expected a host event notification")
        self._event(message)
        return self._events.popleft() if self._events else message

    def artifact(self, relative_path: str, title: str) -> dict[str, str]:
        root = self.output_dir.resolve()
        path = (root / relative_path).resolve()
        try:
            relative = path.relative_to(root)
        except ValueError as error:
            raise ValueError("Artifact must belong to this job's output directory") from error
        if path == root:
            raise ValueError("Artifact must belong to this job's output directory")
        if path.name.lower() == "receipt.json" or path.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("Reserved artifact name or artifact exceeds 64 MiB")
        return dict(title=title, path=relative.as_posix(),
                    sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def run(handler: Callable[[Task], dict[str, Any]], task_type: type[Task] = Task) -> int:
    """Handshake, invoke once, send one result, then exit with its status."""
    task = None
    try:
        write_frame(sys.stdout.buffer, dict(type="hello"))
        task = task_type(read_frame(sys.stdin.buffer))
        result = handler(task)
        task.send("result", result=result)
        return 0
    except Exception as error:
        if task is not None:
            task.send("error", message=f"{type(error).__name__}: {error}")
        else:
            print(f"GUI provider handshake failed: {error}", file=sys.stderr)
        return 1
