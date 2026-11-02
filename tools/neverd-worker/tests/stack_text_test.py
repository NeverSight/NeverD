#!/usr/bin/env python3
"""Both C routes show proven multi-store stack text through the real worker."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from transport_test import Client


MESSAGE = "PublicConfigurationEntry"
HEADER = """typedef unsigned char V16 __attribute__((vector_size(16)));
extern const V16 chunk0, chunk1, chunk2;
__declspec(dllimport) void observe_text(const unsigned short *);
__declspec(dllimport) void observe_wide(const long long *);
"""
INPUT = """#include "types.h"
__declspec(dllexport) void stack_text(void) {
    __declspec(align(16)) unsigned short buffer[25];
    *(V16 *)(buffer + 0) = chunk0;
    *(V16 *)(buffer + 8) = chunk1;
    *(V16 *)(buffer + 16) = chunk2;
    buffer[24] = 0;
    observe_text(buffer);
}
__declspec(dllexport) void unknown_text(unsigned short replacement) {
    __declspec(align(16)) unsigned short buffer[25];
    *(V16 *)(buffer + 0) = chunk0;
    *(V16 *)(buffer + 8) = chunk1;
    *(V16 *)(buffer + 16) = chunk2;
    buffer[24] = 0;
    buffer[9] = replacement;
    observe_text(buffer);
}
__declspec(dllexport) void wide_pointer(void) {
    long long value = 42;
    observe_wide(&value);
}
"""


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (command, result.stdout, result.stderr)


def fixture(root, target, cc, linker):
    (root / "types.h").write_text(HEADER, encoding="utf-8")
    (root / "input.c").write_text(INPUT, encoding="utf-8")
    (root / "provider.c").write_text(
        "__declspec(dllexport) void observe_text(const unsigned short *p) { (void)p; }\n"
        "__declspec(dllexport) void observe_wide(const long long *p) { (void)p; }\n",
        encoding="utf-8")
    data = '#include "types.h"\n'
    encoded = MESSAGE.encode("utf-16le")
    for index in range(3):
        values = ",".join(str(byte) for byte in encoded[index * 16:index * 16 + 16])
        # Separate PE sections prevent any individual image load from already
        # naming the whole text. Only the actual stack stores assemble it.
        data += (f'__declspec(allocate(".s{index}")) __declspec(align(16)) '
                 f'const V16 chunk{index} = {{{values}}};\n')
    (root / "data.c").write_text(data, encoding="utf-8")
    for name in ("provider", "input", "data"):
        run([cc, "--target=" + target, "-O2", "-ffreestanding", "-fno-builtin",
             "-c", str(root / (name + ".c")), "-o", str(root / (name + ".obj"))])
    run([linker, "/dll", "/noentry", "/nodefaultlib",
         "/out:" + str(root / "provider.dll"), "/implib:" + str(root / "provider.lib"),
         str(root / "provider.obj")])
    binary = root / "stack-strings.dll"
    run([linker, "/dll", "/noentry", "/nodefaultlib", "/out:" + str(binary),
         str(root / "input.obj"), str(root / "data.obj"), str(root / "provider.lib")])
    return binary


def check(worker, binary, route, cc):
    with Client(worker) as client:
        opened = client.call("open", {"path": str(binary), "analysis": False, "warm": False})
        assert opened["status"] == "ok", opened
        declared = client.call("local_types_declare", {
            "language": "c",
            "source": "void observe_text(const unsigned short *text);\n"
                      "void stack_text(void);\n"
                      "void observe_wide(const long long *value);\n"
                      "void wide_pointer(void);\n"
                      "void unknown_text(unsigned short replacement);\n"})
        assert declared["status"] == "ok" and not declared["payload"]["problems"], declared
        for function in ("stack_text", "unknown_text"):
            resolved = client.call("resolve", {"query": function})
            assert resolved["status"] == "ok", resolved
            response = client.call("decompile", {
                "address": resolved["payload"]["address"], "representation": route,
                "offset": 0, "limit": 2048})
            assert response["status"] == "ok", response
            payload = response["payload"]
            assert payload.get("complete") and payload.get("next_offset") is None, payload
            text = payload.get("text", "") or "\n".join(
                row if isinstance(row, str) else row.get("text", "")
                for row in payload.get("lines", []))
            assert "extern void observe_text(const uint16_t *text);" in text, text
            body = text[text.rfind(function + "("):]
            comments = [line for line in body.splitlines() if "stack text at 0x" in line]
            expected = function == "stack_text"
            assert any(MESSAGE in line for line in comments) == expected, (route, function, text)
            assert "observe_text(" in body, text
        resolved = client.call("resolve", {"query": "wide_pointer"})
        assert resolved["status"] == "ok", resolved
        response = client.call("decompile", {
            "address": resolved["payload"]["address"], "representation": route,
            "offset": 0, "limit": 2048})
        assert response["status"] == "ok", response
        payload = response["payload"]
        assert payload.get("complete") and payload.get("next_offset") is None, payload
        # The original declaration distinguishes long long from int64_t on
        # LP64 hosts, even though both hold the same number of bytes.
        source = binary.parent / (route + "-wide.c")
        source.write_text(payload["text"] + "\n"
                          "void observe_wide(const long long *value) { (void)value; }\n",
                          encoding="utf-8")
        run([cc, "-std=gnu11", "-ffreestanding", "-fsyntax-only",
             "-Werror=incompatible-pointer-types", str(source)])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    cc = shutil.which(args.cc)
    linker = shutil.which("lld-link")
    if not linker and cc:
        sibling = Path(cc).with_name("lld-link.exe" if Path(cc).suffix == ".exe" else "lld-link")
        if sibling.is_file():
            linker = str(sibling)
    if not cc or not linker:
        print("SKIP: stack text fixtures need Clang and lld-link")
        return 77
    version = subprocess.run([cc, "--version"], capture_output=True, text=True, timeout=10)
    if version.returncode or "clang" not in version.stdout.lower():
        print("SKIP: stack text fixtures need the Clang cross-target frontend")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-worker-stack-text-") as directory:
        root = Path(directory)
        for architecture in ("x86_64", "aarch64"):
            case = root / architecture
            case.mkdir()
            binary = fixture(case, architecture + "-pc-windows-msvc", cc, linker)
            for route in ("c", "llvmc"):
                selected = case / route
                selected.mkdir()
                copy = selected / binary.name
                shutil.copyfile(binary, copy)
                check(args.worker, copy, route, cc)
    print("x64/ARM64 HighC/LLVMC retain complete stack text and reject unknown-byte reconstruction")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
