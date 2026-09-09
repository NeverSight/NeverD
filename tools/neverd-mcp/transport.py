"""Bounded NeverD worker transport; deliberately independent of MCP framing."""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading

MAX_FRAME = 8 * 1024 * 1024


def read_exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.read(size - len(result))
        if not part:
            raise EOFError("NeverD session closed")
        result.extend(part)
    return bytes(result)


def read_frame(stream):
    size = struct.unpack(">I", read_exact(stream, 4))[0]
    if size == 0 or size > MAX_FRAME:
        raise ValueError("Invalid worker frame size")
    value = json.loads(read_exact(stream, size))
    if not isinstance(value, dict):
        raise ValueError("Worker frame must be an object")
    return value


def write_frame(stream, value):
    body = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_FRAME:
        raise ValueError("Worker frame exceeds limit")
    stream.write(struct.pack(">I", len(body)) + body)
    stream.flush()


class Backend:
    """One serialized owner. A reader thread enforces deadlines on all platforms."""

    def __init__(self, *, worker=None, path=None, attach=None, timeout=120):
        self.process = None
        self.socket = None
        self.timeout = timeout
        self.serial = 0
        self.closed = False
        self.messages = queue.Queue(maxsize=32)
        if attach:
            credential_path = Path(attach)
            if os.name != "nt" and credential_path.stat().st_mode & 0o077:
                raise ValueError("Session credential file must be private to its owner")
            credentials = json.loads(credential_path.read_text(encoding="utf-8"))
            endpoint = credentials["endpoint"]
            if os.name == "nt":
                pipe_path = endpoint if endpoint.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + endpoint
                self.input = self.output = open(pipe_path, "r+b", buffering=0)
            else:
                self.socket = socket.socket(socket.AF_UNIX)
                self.socket.settimeout(timeout)
                self.socket.connect(endpoint)
                self.input = self.output = self.socket.makefile("rwb", buffering=0)
            write_frame(self.input, {"protocol_major": 1, "request_id": "auth",
                                    "operation": "authenticate",
                                    "payload": {"token": credentials["token"]}})
        else:
            executable = Path(worker or "")
            if not executable.is_absolute() or not executable.is_file():
                raise ValueError("--worker must name an explicit absolute executable path")
            if not path or not Path(path).is_file():
                raise ValueError("--file must name an existing local input file")
            self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=None)
            self.input, self.output = self.process.stdin, self.process.stdout
        threading.Thread(target=self._read, daemon=True).start()
        try:
            hello = self._next()
            if hello.get("protocol_major") != 1 or hello.get("type") != "hello":
                raise ValueError("Incompatible NeverD worker handshake")
            self.hello = hello
            self.attached = bool(attach)
            if not attach:
                response = self.query("open", {"path": str(Path(path).resolve()), "read_only": True})
                if response.get("status") not in ("ok", "success"):
                    raise RuntimeError("Could not open input: " + str(response.get("error")))
        except BaseException:
            self.close()
            raise

    def _read(self):
        try:
            while not self.closed:
                message = read_frame(self.output)
                if message.get("type") in ("heartbeat", "progress", "revision"):
                    continue  # Idle liveness events must not fill the response queue.
                self.messages.put(message, timeout=1)
        except (OSError, EOFError, ValueError, queue.Full) as error:
            try:
                self.messages.put_nowait(error)
            except queue.Full:
                pass

    def _next(self):
        try:
            value = self.messages.get(timeout=self.timeout)
        except queue.Empty as error:
            self.close()
            raise TimeoutError("NeverD session request timed out") from error
        if isinstance(value, Exception):
            self.close()
            raise value
        return value

    def query(self, operation, payload, revision=None):
        if self.closed:
            raise RuntimeError("NeverD session has ended")
        self.serial += 1
        request_id = str(self.serial)
        request = {"protocol_major": 1, "request_id": request_id,
                   "operation": operation, "payload": payload}
        if revision:
            request["expected_revision"] = revision
        write_frame(self.input, request)
        while True:
            result = self._next()
            if result.get("type") == "fatal":
                self.close()
                raise RuntimeError("NeverD session failed: " + str(result.get("error", result)))
            if result.get("type") == "response" and result.get("request_id") == request_id:
                return result
            # Progress/revision events are allowed, but do not replace the response.

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.socket:
            try:
                self.socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.socket.close()
        if self.process:
            try:
                self.process.stdin.close()
            except OSError:
                pass
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.process.stdout.close()
        elif hasattr(self, "input"):
            self.input.close()
