#!/usr/bin/env python3
"""Generated PE: direct and frame-cached imports retain their complete ABI."""
from pathlib import Path
import argparse
import re
import struct
import subprocess
import sys
import tempfile

from pe_browse_test import image
from transport_test import Client


ENTRY = 0x140001000
SLOT = 0x140002180


def fixture(cached):
    binary, _ = image()
    data = bytearray(binary)
    struct.pack_into("<H", data, 0x98 + 68, 1)  # Native PE driver subsystem.
    data[0x480:0x480 + 13] = b"ntoskrnl.exe\0"
    struct.pack_into("<Q", data, 0x500, 0x20C0)
    struct.pack_into("<Q", data, 0x580, 0x20C0)
    name = b"\0\0IoCreateDevice\0"
    data[0x4C0:0x4C0 + len(name)] = name
    code = bytearray.fromhex("4883ec68")

    def relative(prefix, address):
        code.extend(prefix)
        code.extend(struct.pack("<i", address - (ENTRY + len(code) + 4)))

    if cached:
        relative(bytes.fromhex("488b05"), SLOT)
        code.extend(bytes.fromhex("4889442460"))
    # RCX forwards the incoming object. The last three arguments live in
    # the actual Win64 outgoing area, with independent 4/1/8-byte widths.
    code.extend(bytes.fromhex("31d2 4c8d442440 41b922000000 "
                              "c744242000000000 c644242800"))
    relative(bytes.fromhex("488d05"), 0x140002160)
    code.extend(bytes.fromhex("4889442430"))
    if cached:
        code.extend(bytes.fromhex("488b442460 ffd0"))
    else:
        relative(bytes.fromhex("ff15"), SLOT)
    # Transport of a volatile register is not an RAX:RDX aggregate result.
    code.extend(bytes.fromhex("4889542458 4883c468 c3"))
    data[0x200:0x200 + len(code)] = code
    return bytes(data)


def cached_helpers_fixture():
    binary, _ = image()
    data = bytearray(binary)
    code = bytearray.fromhex("4883ec38")
    for target in (ENTRY + 0x100, ENTRY + 0x120):
        code.extend(bytes.fromhex("48b8") + struct.pack("<Q", target))
        code.extend(bytes.fromhex("4889442428 ba77000000 ff542428"))
    code.extend(bytes.fromhex("4883c438 c3"))
    data[0x200:0x200 + len(code)] = code
    data[0x300:0x306] = bytes.fromhex("b807000000c3")
    data[0x320:0x328] = bytes.fromhex("4889c84883c001c3")
    return bytes(data)


def check_cached_helpers(worker, directory):
    path = Path(directory) / "cached-helpers.exe"
    binary = cached_helpers_fixture()
    path.write_bytes(binary)
    for prepared in (False, True):
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False,
                                "read_only": True})
            if prepared:
                for address in (ENTRY + 0x100, ENTRY + 0x120):
                    ok(client, "decompile", {"address": hex(address),
                                             "representation": "c"})
            for representation in ("c", "llvmc"):
                text = ok(client, "decompile", {"address": hex(ENTRY),
                          "representation": representation})["text"]
                body = text[text.index("{", text.index("start(")):]
                for target, arity in ((ENTRY + 0x100, 0), (ENTRY + 0x120, 1)):
                    calls = re.findall(
                        rf"^\s*(?!extern\b)[^\n]*\bsub_{target:X}\(([^;]*)\);",
                        body, re.M | re.I)
                    assert len(calls) == 1, (prepared, representation, text)
                    assert bool(calls[0].strip()) == bool(arity), text
                    assert "," not in calls[0], text
    assert path.read_bytes() == binary


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def call_arguments(text):
    # Ignore declarations. Count commas only at the call's own nesting level.
    body = text
    match = next((m for m in re.finditer(r"\bIoCreateDevice\s*\(", body)
                  if not body[body.rfind("\n", 0, m.start()) + 1:m.start()]
                  .lstrip().startswith("extern ")), None)
    if match:
        first = match.end()
    else:
        # LLVM-C may retain the already loaded mutable callable as a typed
        # indirect target. Its complete signature and actual operands still
        # have to agree with the import contract.
        indirect = re.search(r"\b(?:call|v)\d+\s*=\s*([^;]+);", body)
        assert indirect, text
        marker = indirect.group(1).rfind("))(")
        if marker >= 0:
            first = indirect.start(1) + marker + 3
        else:
            # A captured pointer with a complete declaration needs no cast
            # at the call. Its runtime value is still the original capture.
            named = re.match(r"[A-Za-z_]\w*\s*\(", indirect.group(1))
            assert named, text
            first = indirect.start(1) + named.end()
    depth, args = 1, []
    for i in range(first, len(body)):
        ch = body[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if not depth:
                return args + [body[first:i].strip()]
        elif ch == "," and depth == 1:
            args.append(body[first:i].strip())
            first = i + 1
    raise AssertionError(text)


def run(worker, compiler=None):
    with tempfile.TemporaryDirectory(prefix="neverd-cached-abi-") as directory:
        check_cached_helpers(worker, directory)
        for cached in (False, True):
            path = Path(directory) / ("cached.sys" if cached else "direct.sys")
            binary = fixture(cached)
            path.write_bytes(binary)
            with Client(worker) as client:
                ok(client, "open", {"path": str(path), "analysis": False})
                for representation in ("c", "llvmc"):
                    text = ok(client, "decompile", {"address": hex(ENTRY),
                              "representation": representation,
                              "limit": 2048})["text"]
                    arguments = call_arguments(text)
                    assert len(arguments) == 7, (cached, representation, text)
                    assert re.search(r"\barg\d+\b", arguments[0]), (cached, representation, text)
                    assert "unknown" not in arguments[0] and "__builtin_trap" not in arguments[0], text
                    assert "0x22" in arguments[3], arguments
                    assert "__int128" not in text, text
                    if representation == "c":
                        assert "IoCreateDevice" in text, text
                    if compiler:
                        source = Path(directory) / f"{cached}-{representation}.c"
                        source.write_text(text, encoding="utf-8")
                        if Path(compiler).stem.lower() in ("cl", "clang-cl"):
                            command = [compiler, "/nologo", "/TC", "/std:c11", "/Zs", str(source)]
                        else:
                            command = [compiler, "-std=c11", "-fsyntax-only", str(source)]
                        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                        assert result.returncode == 0, result.stdout + result.stderr + text
            assert path.read_bytes() == binary
    print("direct/cached Win64 imports: all seven operands and scalar result passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc")
    arguments = parser.parse_args()
    run(arguments.worker, arguments.cc)
