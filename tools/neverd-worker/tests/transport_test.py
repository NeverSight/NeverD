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
        return self.next(lambda message: message.get("type") == "response"
                         and message.get("request_id") == request_id
                         and message.get("status") != "progress")

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
        # The ways a file can be loaded, listed before it is opened: what the
        # engine reads the file as, then the binary file any file is.
        assert "identify" in client.hello["capabilities"]
        rows = client.call("identify", {"path": str(binary)})["payload"]["rows"]
        assert [(row["loader"], row["loadable"]) for row in rows] == [("fixture", True), ("binary", True)], rows
        missing = client.call("identify", {"path": str(binary) + ".missing"})
        assert missing["error"]["code"] == "invalid_request", missing
        # A binary file opens as the load dialog places it, and the engine
        # keeps that choice with the input for the next open.
        firmware = Path(directory) / "firmware.bin"
        firmware.write_bytes(b"\x90\xc3")
        placed = clients.enter_context(Client(executable))
        for wrong, code in (({"loader": "mips"}, "invalid_request"),
                            ({"loader": "binary", "processor": "mips"}, "invalid_request"),
                            ({"loader": "binary", "processor": "x86_64", "base": "start"}, "invalid_address")):
            refused = placed.call("open", {"path": str(firmware), **wrong})
            assert refused["error"]["code"] == code, (wrong, refused)
        assert placed.call("open", {"path": str(firmware), "loader": "binary", "processor": "x86_64",
                                    "base": "0x400000"})["status"] == "ok"
        kept = json.loads(Path(str(firmware) + ".neverd/load.json").read_text())
        assert kept == {"loader": "binary", "processor": "x86_64", "base": "0x400000"}, kept
        assert placed.call("open", {"path": str(firmware), "loader": "evm"})["status"] == "ok"
        assert json.loads(Path(str(firmware) + ".neverd/load.json").read_text()) == {"loader": "evm"}
        flag = client.call("open", {"path": str(binary), "analysis": "yes"})
        assert flag["error"]["code"] == "invalid_request", flag
        open_id = client.send("open", {"path": str(binary)}, fragmented=True)
        opened = client.response(open_id)
        assert opened["status"] == "ok", opened
        for phase, done in (("image", 0), ("ready", 1)):
            progress = client.next(lambda message: message.get("type") == "response"
                                   and message.get("request_id") == open_id
                                   and message.get("status") == "progress")
            assert progress["operation"] == "open", progress
            payload = progress["payload"]
            assert (payload["phase"], payload["done"], payload["total"]) == (phase, done, 1), progress
            assert Path(payload["detail"]).resolve() == binary.resolve(), progress
        revision = opened["revision"]
        assert opened["payload"]["entry_address"] == BASE
        assert opened["payload"]["function_count"] == 600
        assert isinstance(opened["background_sequence"], str), opened
        assert int(opened["background_sequence"]) > 0, opened
        assert isinstance(opened["background"], dict), opened
        snapshot_reply = client.call("overview", {"buckets": 8})
        assert int(snapshot_reply["background_sequence"]) > int(opened["background_sequence"]), snapshot_reply
        assert snapshot_reply["background"]["generation"] == snapshot_reply["payload"]["generation"], snapshot_reply
        assert snapshot_reply["background"]["functions"] == 600, snapshot_reply
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
        assert not Path(str(binary) + ".neverd/annotations.json").exists()
        assert client.call("save")["payload"]["saved"]
        saved = json.loads(Path(str(binary) + ".neverd/annotations.json").read_text(encoding="utf-8"))
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
        assert int(heartbeat["background_sequence"]) >= int(snapshot_reply["background_sequence"]), heartbeat
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
        # Whole-program analysis brings the switch tables: a table under its
        # jpt_ name with a slot per line, the load and the dispatch commented,
        # and each target referred to from the dispatch and the table.
        table = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012343114", "before": 0, "after": 6})["payload"]["lines"]]
        assert table[0].startswith("jpt_FFFF800012340086 dd offset loc_FFFF800012340088 - "), table
        assert any("jump table for switch statement" in line for line in table), table
        assert any(line.startswith("dd offset loc_FFFF80001234008A - ") for line in table), table
        code = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012340085", "before": 0, "after": 3})["payload"]["lines"]
            if line["kind"] == "insn"]
        assert code[0].endswith("; switch 3 cases") and code[1].endswith("; switch jump"), code
        target = client.call("xrefs", {"address": "0xffff800012340088", "direction": "to"})["payload"]["items"]
        assert {(item["address"], item["type"]) for item in target} >= {
            ("0xffff800012340086", "j"), ("0xffff800012343114", "o")}, target
        # A mangled name reads demangled above its function and beside the
        # instructions that name it, and the Functions window shows it so.
        header = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012340090", "before": 0, "after": 20})["payload"]["lines"]]
        assert "; Widget::draw()" in header, header
        assert any(line.startswith("jmp") and line.endswith("; Widget::draw()") for line in header), header
        rows = client.call("functions", {"filter": "widget::draw"})["payload"]["items"]
        assert [row["name"] for row in rows] == ["function_9"], rows
        tables = client.call("names", {"filter": "jpt_"})["payload"]["items"]
        assert [(item["name"], item["address"]) for item in tables] == [
            ("jpt_FFFF800012340086", "0xffff800012343114")], tables
        first = client.call("decompile", {"address": BASE, "representation": "llvm", "limit": 400})["payload"]
        last = client.call("decompile", {"address": BASE, "representation": "llvm", "offset": first["next_offset"], "limit": 400})["payload"]
        assert first["total_lines"] == 700 and not first["complete"] and last["complete"]
        assert "code line 400" in last["text"]
        assert first["mapping_status"] == "unsupported_representation" and first["rows"] == []
        # C pages say where the definition begins after the prelude.
        source = client.call("decompile", {"address": BASE, "representation": "llvmc", "limit": 2})["payload"]
        assert source["prelude"] == {"lines": 7, "end_byte": 207}, source
        assert source["text"] == "#include <stdint.h>\ntypedef struct QDomNode QDomNode;\n", source
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
        # Isolate the mock's oversized graph entry from the other protocol
        # state. An interior byte selects the ordinary owner until defined.
        graph_binary = Path(directory) / "cfg-budget.bin"
        graph_binary.write_bytes(b"benign graph fixture")
        with Client(executable) as graph_client:
            assert graph_client.call("open", {"path": str(graph_binary)})["status"] == "ok"
            interior = "0xffff800012340001"
            assert len(graph_client.call("cfg", {"address": interior})["payload"]["nodes"]) == 2
            assert graph_client.call("function_create", {"address": interior})["status"] == "ok"
            assert graph_client.call("cfg", {"address": interior})["status"] == "budget_exceeded"
            assert graph_client.call("function_delete", {"address": interior})["status"] == "ok"
            assert len(graph_client.call("cfg", {"address": interior})["payload"]["nodes"]) == 2
        # Direct references come from the instruction index without analysis.
        direct = client.call("xrefs", {"address": BASE})["payload"]
        assert direct["items"][0]["address"] == "0xffff800012340008" and direct["items"][0]["type"] == "j", direct
        assert client.call("xrefs", {"address": BASE, "direction": "from"})["status"] == "ok"
        # Idle work reports the function count after discovery.
        beat = client.next(lambda item: item.get("type") == "heartbeat" and "functions" in item.get("background", {}))
        assert beat["background"]["functions"] == 600, beat
        # A call through a relocated slot, and the slot's own pointer.
        through_slot = client.call("xrefs", {"address": "0xffff800012340020"})["payload"]["items"]
        kinds = {(item["from"], item["kind"], item["type"]) for item in through_slot}
        assert ("0xffff800012340034", "icall", "p") in kinds, through_slot
        assert ("0xffff800012343008", "offset", "o") in kinds, through_slot
        data = client.call("listing", {"address": "0xffff800012343000", "after": 20})["payload"]["lines"]
        slot = next(line for line in data if line["kind"] == "data")
        assert "dq" in slot["text"] and "offset function_1" in slot["text"], data
        code = [line["text"] for line in client.call("listing", {"address": BASE, "after": 120})["payload"]["lines"]]
        assert any(text.strip() == "; __unwind {" for text in code), code
        assert any(text.strip() == "; } // starts at FFFF800012340000" for text in code), code
        assert any(text.strip() == "; __unwind { // __gxx_personality_v0" for text in code), code
        # function_5 has no frame pointer: its stack variables are named from
        # the tracked stack pointer, except where no path reaches.
        frame = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012340050", "before": 0, "after": 40})["payload"]["lines"]]
        for expected in ("var_20 = xmmword ptr -20h", "var_10 = qword ptr -10h", "arg_0 = qword ptr 8",
                         "mov [rsp+28h+var_10], rax", "lea rdi, [rsp+28h+var_20]",
                         "mov rax, [rsp+28h+arg_0]", "movups [rsp+28h+var_20], xmm0",
                         "mov rax, [rsp+8]"):
            assert any(text.endswith(expected) for text in frame), (expected, frame)
        assert not any("bp-based frame" in text for text in frame), frame
        # function_6 names the slot its single path reaches, and leaves raw the
        # operand two paths reach at different stack depths.
        join = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012340060", "before": 0, "after": 30})["payload"]["lines"]
            if line["kind"] == "insn"]
        assert "mov rax, [rsp+8+var_8]" in join, join
        assert "mov rax, [rsp+8]" in join, join
        # Strings in every encoding the engine finds, under the classic forms.
        encodings = client.call("string_encodings")["payload"]["items"]
        assert [e["name"] for e in encodings] == ["ascii", "utf-8", "utf-16le", "gbk", "big5"], encodings
        assert [e.get("legacy", False) for e in encodings] == [False, False, False, True, True], encodings
        options = client.call("string_options")["payload"]
        assert options == {"encodings": ["ascii", "utf-8", "utf-16le"], "preferred": None, "min_length": 4}, options
        rodata = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012343100", "before": 0, "after": 12})["payload"]["lines"]]
        assert any(line.startswith("asc_FFFF800012343100 db '\u4e2d\u6587',0") for line in rodata), rodata
        assert any(line.startswith("aWide:") for line in rodata), rodata
        assert "text \"UTF-16LE\", 'Wide',0" in rodata, rodata
        types = {row["text"]: row["type"] for row in client.call("strings", {"filter": ""})["payload"]["items"]}
        assert types.get("Wide") == "UTF-16LE" and types.get("\u4e2d\u6587") == "UTF-8", types
        assert client.call("string_options", {"encodings": ["ascii", "utf-8"]})["payload"]["encodings"] == ["ascii", "utf-8"]
        rodata = [line["text"] for line in client.call(
            "listing", {"address": "0xffff800012343100", "before": 0, "after": 12})["payload"]["lines"]]
        assert not any("UTF-16LE" in text for text in rodata), rodata
        assert client.call("string_options", {"encodings": ["ebcdic"]})["error"]["code"] == "unsupported_encoding"
        # Unwritten data: stderr is as large as its symbol, not a run to the
        # end of .bss, and data the code reads has a line of its own, named
        # by the width of the read.
        assert client.call("xrefs", {"address": "0xffff800012343230"})["status"] == "ok"
        bss = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012343200", "before": 0, "after": 16})["payload"]["lines"]]
        assert "stderr dq ?" in bss, bss
        assert "db 28h dup(?)" in bss, bss
        assert any(line.startswith("qword_FFFF800012343230 dq ? ; DATA XREF: function_7+6") for line in bss), bss
        # The user's data items: D makes a value and, again, the next size; U
        # shows the item's bytes as bytes; A the string that starts there.
        # Each is one step of history, and code is no item's.
        assert "item_define" in client.hello["capabilities"]
        item = {"address": "0xffff800012343230", "action": "data"}
        assert client.call("item_define", item)["payload"]["kind"] == "byte"
        assert client.call("item_define", item)["payload"]["kind"] == "word"
        word_rows = client.call("listing", {
            "address": item["address"], "before": 0, "after": 2})["payload"]["lines"]
        extent_rows = [row for row in word_rows if row["item"] == item["address"]]
        assert extent_rows and all(row["item_size"] == "0x2" for row in extent_rows), word_rows
        bss = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012343230", "before": 0, "after": 2})["payload"]["lines"]]
        assert any(line.startswith("word_FFFF800012343230 dw ?") for line in bss), bss
        undefined = client.call("item_define", {"address": "0xffff800012343230", "action": "undefine"})["payload"]
        assert undefined["kind"] == "undefined" and undefined["size"] == 2, undefined
        bss = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012343230", "before": 0, "after": 2})["payload"]["lines"]]
        assert any(line.startswith("unk_FFFF800012343230 db ?") for line in bss), bss
        assert client.call("undo")["status"] == "ok"
        string = client.call("item_define", {"address": "0xffff800012343108", "action": "string"})["payload"]
        assert string == {"address": "0xffff800012343108", "kind": "string", "size": 10, "saved": True}, string
        string_rows = client.call("listing", {
            "address": string["address"], "before": 0, "after": 8})["payload"]["lines"]
        extent_rows = [row for row in string_rows if row["item"] == string["address"]]
        assert extent_rows and all(row["item_size"] == "0xa" for row in extent_rows), string_rows
        refused = client.call("item_define", {"address": BASE, "action": "data"})
        assert refused["error"]["code"] == "invalid_request", refused
        steps = [(step["kind"], step["address"]) for step in client.call("history")["payload"]["items"]
                 if step["applied"] and step["kind"] == "item"]
        assert steps == [("item", "0xffff800012343230")] * 2 + [("item", "0xffff800012343108")], steps
        # N names any address: data takes the name in its label, and the name
        # resolves to it; a name used elsewhere or in an automatic form is
        # refused, and undo takes the name back.
        assert client.call("rename", {"address": "0xffff800012343230", "name": "program_name"})["status"] == "ok"
        bss = [" ".join(line["text"].split()) for line in client.call(
            "listing", {"address": "0xffff800012343230", "before": 0, "after": 2})["payload"]["lines"]]
        assert any(line.startswith("program_name dw ?") for line in bss), bss
        resolved = client.call("resolve", {"query": "program_name"})["payload"]
        assert resolved["address"] == "0xffff800012343230" and resolved["address_name"] == "program_name", resolved
        used = client.call("rename", {"address": "0xffff800012343200", "name": "program_name"})
        assert used["error"]["code"] == "invalid_request", used
        automatic = client.call("rename", {"address": "0xffff800012343200", "name": "sub_1234"})
        assert automatic["error"]["code"] == "invalid_request", automatic
        assert client.call("undo")["status"] == "ok"
        assert client.call("resolve", {"query": "program_name"})["status"] == "error"
        # Operand types: an instruction's operand shows its number in the base
        # the user picks, with its sign changed; each is a step of history.
        assert "operand_format" in client.hello["capabilities"]

        def operand_line():
            lines = client.call("listing", {"address": "0xffff800012340051", "before": 0, "after": 1})
            return next(" ".join(line["text"].split()) for line in lines["payload"]["lines"]
                        if "sub" in line["text"])
        assert "sub rsp, 20h" in operand_line(), operand_line()
        sub = {"address": "0xffff800012340051", "operand": 1}
        assert client.call("operand_format", {**sub, "action": "decimal"})["status"] == "ok"
        assert "sub rsp, 32" in operand_line(), operand_line()
        negated = client.call("operand_format", {**sub, "action": "negate"})["payload"]
        assert negated["format"] == {"base": "decimal", "negate": True, "invert": False}, negated
        assert "sub rsp, -18446744073709551584" in operand_line(), operand_line()
        data = client.call("operand_format", {"address": "0xffff800012343100", "operand": 0, "action": "hex"})
        assert data["error"]["code"] == "invalid_request", data
        assert client.call("undo")["status"] == "ok" and client.call("undo")["status"] == "ok"
        assert "sub rsp, 20h" in operand_line(), operand_line()
        # An operand that is no number gives the format to the instruction's
        # last number; an instruction without one is refused.  A format
        # outlives the session: its sidecar is read back on open.
        fallback = client.call("operand_format", {**sub, "operand": 0, "action": "binary"})
        assert fallback["payload"]["operand"] == 1, fallback
        assert "sub rsp, 100000b" in operand_line(), operand_line()
        bare = client.call("operand_format", {"address": "0xffff800012340050", "action": "hex"})
        assert bare["error"]["code"] == "invalid_request", bare
        assert "no number" in bare["error"]["message"], bare
        client.call("string_options", {"encodings": ["ascii", "utf-8", "utf-16le"]})
        # Hex views ask for bytes as text in an encoding, a cell per byte.
        bytes_ = client.call("bytes", {"address": "0xffff800012343000", "size": 4, "text_encoding": "ascii"})["payload"]
        assert len(bytes_["cells"]) == 4, bytes_
        assert client.call("bytes", {"address": "0xffff800012343000", "size": 4,
                                     "text_encoding": "klingon"})["error"]["code"] == "unsupported_encoding"
        assert "string_references" in client.hello["capabilities"]
        # Instructions that refer to strings, directly or through a slot.
        refs = client.call("string_references")["payload"]
        rows = {(row["address"], row["text"], row["type"], row.get("via")) for row in refs["items"]}
        assert refs["total"] == 2, refs
        assert ("0xffff800012340072", "\u4e2d\u6587", "UTF-8", None) in rows, refs
        assert ("0xffff800012340073", "Wide", "UTF-16LE", "0xffff800012343038") in rows, refs
        assert all(row["function"] == "function_7" for row in refs["items"]), refs
        assert client.call("string_references", {"filter": "WIDE"})["payload"]["total"] == 1
        assert client.call("string_references", {"filter": "340073"})["payload"]["total"] == 1
        assert client.call("string_references", {"sort": "size"})["error"]["code"] == "invalid_request"
        by_text = client.call("string_references", {"sort": "text", "descending": True})["payload"]["items"]
        assert [row["text"] for row in by_text] == ["\u4e2d\u6587", "Wide"], by_text
        # A reference into a string reads it from the first whole character
        # on, once that is long enough to be a string itself.
        client.call("string_options", {"encodings": ["ascii", "utf-8", "utf-16le"], "min_length": 1})
        tails = client.call("string_references")["payload"]
        rows = {(row["address"], row["string_address"], row["text"]) for row in tails["items"]}
        assert tails["total"] == 4, tails
        assert ("0xffff800012340074", "0xffff800012343103", "\u6587") in rows, tails
        assert ("0xffff800012340075", "0xffff80001234310b", "de") in rows, tails
        # Several code pages are searched together; the preferred one reads
        # first, and only a code page can be preferred.
        both = client.call("string_options", {"encodings": ["gbk", "big5"], "preferred": "big5"})["payload"]
        assert both["encodings"] == ["gbk", "big5"] and both["preferred"] == "big5", both
        assert client.call("string_options", {"preferred": "utf-8"})["error"]["code"] == "invalid_request"
        assert client.call("string_options", {"preferred": "klingon"})["error"]["code"] == "unsupported_encoding"
        client.call("string_options", {"encodings": ["ascii", "utf-8", "utf-16le"], "min_length": 4})
        # The strings filter also matches addresses and types.
        assert client.call("strings", {"filter": "343108"})["payload"]["total"] == 1
        assert client.call("strings", {"filter": "utf-16"})["payload"]["total"] == 1
        # IR constant references require whole-program analysis.
        assert client.call("xrefs", {"address": BASE, "source": "ir"})["payload"]["items"][0]["address"] == "0xffff800012340008"
        client.close()
        assert b"native stdout" in client.logs and b"python-style print" in client.logs
        assert client.process.returncode == 0

        # OS releases the writer lock on exit, and acknowledged edits survive.
        reopened = clients.enter_context(Client(executable))
        assert reopened.call("open", {"path": str(binary)})["status"] == "ok"
        assert reopened.call("annotations")["payload"]["items"][0]["text"] == note
        assert reopened.call("functions", {"filter": "renamed_function"})["payload"]["total"] == 1
        # The item is the user's; its label waits for the reference index.
        assert reopened.call("xrefs", {"address": "0xffff800012343230"})["status"] == "ok"
        bss = [" ".join(line["text"].split()) for line in reopened.call(
            "listing", {"address": "0xffff800012343230", "before": 0, "after": 2})["payload"]["lines"]]
        assert any(line.startswith("word_FFFF800012343230 dw ?") for line in bss), bss
        code = [" ".join(line["text"].split()) for line in reopened.call(
            "listing", {"address": "0xffff800012340051", "before": 0, "after": 1})["payload"]["lines"]]
        assert any("sub rsp, 100000b" in line for line in code), code
        reopened.close()
        # function_6's bytes are a part of function_0 apart from its body: the
        # chunk opens and closes with its function's name, reads by a label,
        # and its lines belong to that function.
        chunked = Path(directory) / "function-chunks.bin"
        chunked.write_bytes(b"mock input")
        parts = clients.enter_context(Client(executable))
        assert parts.call("open", {"path": str(chunked)})["status"] == "ok"
        lines = parts.call("listing", {"address": "0xffff800012340060", "before": 4, "after": 30})["payload"]["lines"]
        texts = [" ".join(line["text"].split()) for line in lines]
        assert "; START OF FUNCTION CHUNK FOR function_0" in texts, texts
        assert any(text.startswith("loc_FFFF800012340060:") for text in texts), texts
        assert "; END OF FUNCTION CHUNK FOR function_0" in texts, texts
        assert not any("function_6" in text for text in texts), texts
        chunk = [line for line in lines
                 if line["kind"] == "insn" and 0xffff800012340060 <= int(line["address"], 16) < 0xffff800012340070]
        assert chunk and all(line["function"] == "function_0" and line["function_address"] == BASE
                             for line in chunk), chunk
        listed = parts.call("functions", {"filter": "function_6", "limit": 50})["payload"]["items"]
        assert listed and not any(item["address"] == "0xffff800012340060" for item in listed), listed
        # An address in the chunk resolves to its function, so a reference
        # there opens that function's code.
        owner = parts.call("resolve", {"query": "0xffff800012340064"})["payload"]
        assert owner["function_address"] == BASE and owner["address"] == "0xffff800012340064", owner
        parts.close()
        # A symbol whose name spells its string literal (MSVC's ??_C@) leaves
        # the string its own name; a name the user gives it is a name again.
        literals = Path(directory) / "literal-names.bin"
        literals.write_bytes(b"mock input")
        spelled = clients.enter_context(Client(executable))
        assert spelled.call("open", {"path": str(literals)})["status"] == "ok"

        def wide_lines():
            return [" ".join(line["text"].split()) for line in spelled.call(
                "listing", {"address": "0xffff800012343100", "before": 0, "after": 12})["payload"]["lines"]]
        rodata = wide_lines()
        assert any(line.startswith("aWide:") for line in rodata), rodata
        assert not any("??_C@" in line for line in rodata), rodata
        # A literal among the code lists as a string there too.
        code = [" ".join(line["text"].split()) for line in spelled.call(
            "listing", {"address": "0xffff800012340200", "before": 0, "after": 4})["payload"]["lines"]]
        assert any(line.startswith("aInline db 'Inline',0") for line in code), code
        assert not any("??_C@" in line for line in code), code
        assert spelled.call("rename", {"address": "0xffff800012343108", "name": "kWide"})["status"] == "ok"
        rodata = wide_lines()
        assert any(line.startswith("kWide:") for line in rodata), rodata
        assert "text \"UTF-16LE\", 'Wide',0" in rodata, rodata
        spelled.close()

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
