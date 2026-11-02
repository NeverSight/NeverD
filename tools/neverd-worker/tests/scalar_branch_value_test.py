#!/usr/bin/env python3
"""Execute recovered scalar transfers through both source routes on Linux x64."""
import ctypes
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile

from transport_test import Client


ASSEMBLY = """
.intel_syntax noprefix
.text
.globl dispatch_probe
.type dispatch_probe,@function
dispatch_probe:
.Lorigin:
COMPUTE
lea rax,[rip+BASE]
add rax,rcx
jmp rax
.byte 0xe9
.Ldone:
mov eax,0x42
ret
.Lbad:
mov eax,7
ret
.Lbase:
.size dispatch_probe,.-dispatch_probe
.section .rodata
.Ldelta: .long .Ldone-.Lorigin
.section .note.GNU-stack,"",@progbits
"""

IDENTITY_OFFSET = ".byte 0x48,0x81,0xc1\n.long .Ldone-.Lorigin"
INPUTS = (0, 1, 2, 3, 7, 255, 256, 0x5555555555555555,
          0xaaaaaaaaaaaaaaaa, 0x7fffffffffffffff, 0x8000000000000000,
          0xffffffffffffffff)

CASES = (
    ("load", "mov ecx,DWORD PTR[rip+.Ldelta]", 19),
    ("local-target-long-prefix", ".rept 4096\nmov r10,0x123456789abcdef\n"
     "jmp 1f\n.byte 0xcc\n1:\n.endr\nmov ecx,DWORD PTR[rip+.Ldelta]",
     4096 * 13 + 19),
    ("joined-prefix", "test rdi,rdi\njz .Lother\nmov ecx,DWORD PTR[rip+.Ldelta]\n"
     "jmp .Lcontinued\n.Lother:\nmov ecx,DWORD PTR[rip+.Ldelta]\n.Lcontinued:", 32),
    ("direct-prefix", "mov ecx,DWORD PTR[rip+.Ldelta]\njmp .Lcontinued\n"
     ".byte 0xcc\n.Lcontinued:", 22),
    ("symbolic-prefix", "mov rcx,rdi\nor rcx,rsi\nmov rdx,rdi\nand rdx,rsi\n"
     "jmp .Lcontinued\n.byte 0xcc\n.Lcontinued:\n"
     "add rcx,rdx\nsub rcx,rdi\nsub rcx,rsi\n" + IDENTITY_OFFSET, 44),
    ("immutable-identity", "mov ecx,DWORD PTR[rip+.Ldelta]\nxor rcx,rdi\n"
     "mov edx,DWORD PTR[rip+.Ldelta]\nxor rcx,rdx\nxor rcx,rdi\n" +
     IDENTITY_OFFSET, 41),
    ("not", ".byte 0xb9\n.long ~(.Ldone-.Lorigin)\nnot ecx", 20),
    ("neg", ".byte 0xb9\n.long -(.Ldone-.Lorigin)\nneg ecx", 20),
    ("sext", ".byte 0xb9\n.long .Ldone-.Lbase\nmovsxd rcx,ecx", 21),
    ("self-sub", "sub ecx,ecx\n.byte 0x81,0xc1\n.long .Ldone-.Lorigin", 21),
    ("self-sub-flags", "sub rcx,rcx\njc .Lbad\n.byte 0xb9\n.long .Ldone-.Lorigin", 23),
    ("self-cmp-flags", "cmp rcx,rcx\njnz .Lbad\n.byte 0xb9\n.long .Ldone-.Lorigin", 23),
    ("or-and", "mov rcx,rdi\nor rcx,rsi\nmov rdx,rdi\nand rdx,rsi\n"
     "add rcx,rdx\nsub rcx,rdi\nsub rcx,rsi\n" + IDENTITY_OFFSET, 41),
    ("xor-and", "mov rcx,rdi\nxor rcx,rsi\nmov rdx,rdi\nand rdx,rsi\n"
     "shl rdx,1\nadd rcx,rdx\nsub rcx,rdi\nsub rcx,rsi\n" + IDENTITY_OFFSET, 44),
    ("or-not", "mov rcx,rdi\nnot rcx\nand rcx,rsi\nmov rdx,rdi\nor rdx,rsi\n"
     "sub rdx,rcx\nmov rcx,rdx\nsub rcx,rdi\n" + IDENTITY_OFFSET, 44),
    ("mask-add", "mov rcx,rdi\nand rcx,0x55555555\nmov rdx,rdi\nand rdx,-1431655766\n"
     "add rcx,rdx\nsub rcx,rdi\n" + IDENTITY_OFFSET, 46),
)


def ok(client, operation, payload):
    response = client.call(operation, payload)
    assert response["status"] == "ok", response
    return response["payload"]


def run(worker, compiler):
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        print("SKIP: native ELF x64 source execution requires Linux x64")
        return 77
    version = subprocess.run([compiler, "--version"], capture_output=True, text=True)
    if version.returncode or "clang" not in version.stdout.lower():
        print("SKIP: generated source requires Clang C extensions")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-scalar-branch-") as temporary:
        root = Path(temporary)
        for name, compute, offset in CASES:
            area = root / name
            area.mkdir()
            assembly, binary = area / "fixture.s", area / "fixture.so"
            assembly.write_text(ASSEMBLY.replace("BASE", ".Lbase" if name == "sext" else ".Lorigin")
                                .replace("COMPUTE", compute))
            subprocess.run([compiler, "-shared", "-nostdlib", "-Wl,-Bsymbolic",
                            "-Wl,-Ttext-segment=0x400000", str(assembly), "-o", str(binary)],
                           check=True)
            # Run the independently authored machine code in its own process;
            # neither NeverD nor the generated source supplies this oracle.
            subprocess.run([sys.executable, __file__, "--native", str(binary)], check=True)
            with Client(worker) as client:
                ok(client, "open", {"path": str(binary), "analysis": False, "warm": False})
                entry = ok(client, "resolve", {"query": "dispatch_probe"})["address"]
                target = hex(int(entry, 0) + offset)
                decoy = hex(int(target, 0) - 1)
                listing = ok(client, "listing", {"address": decoy, "recover_address": target,
                             "before": 0, "after": 12})
                assert any(row["address"] == decoy and row["kind"] == "data"
                           and "db" in row["text"] and "0E9h" in row["text"]
                           for row in listing["lines"]), (name, listing)
                assert any(row["address"] == target and row["kind"] == "insn"
                           and row.get("function_address") == entry
                           for row in listing["lines"]), (name, listing)
                references = ok(client, "xrefs", {"address": hex(int(target, 0) - 3),
                                "direction": "from"})
                assert any(row["to"] == target and row["kind"] == "jump"
                           for row in references["items"]), (name, references)
                for route in ("c", "llvmc"):
                    source = ok(client, "decompile", {"address": entry, "representation": route,
                                "offset": 0, "limit": 2048})
                    assert source["complete"], source
                    unit = area / (route + ".c")
                    definition = re.search(r"\bdispatch_probe\(([^)]*)\)\s*\{", source["text"])
                    assert definition, source
                    parameters = definition.group(1).strip()
                    count = 0 if parameters in ("", "void") else len(parameters.split(","))
                    assert count <= 2, (name, route, parameters)
                    arguments = ", ".join(("a", "b")[:count])
                    unit.write_text(source["text"] + "\nstatic uint64_t inputs[] = {" +
                                    ",".join(hex(value) + "ULL" for value in INPUTS) + "};\n" +
                                    "int main(void) { for (unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) "
                                    "for (unsigned j=0;j<sizeof(inputs)/sizeof(inputs[0]);++j) { "
                                    "uint64_t a=inputs[i],b=inputs[j]; if (dispatch_probe(" +
                                    arguments + ") != 66) return 1; } return 0; }\n")
                    for optimization in ("-O0", "-O2"):
                        executable = area / (route + optimization)
                        subprocess.run([compiler, "-std=gnu11", optimization, "-fno-strict-aliasing",
                                        "-fsanitize=undefined", "-fsanitize-trap=all", str(unit),
                                        "-o", str(executable)], check=True)
                        subprocess.run([str(executable)], check=True, timeout=10)
                print(name, "native + HighC/LLVMC O0/O2 passed", flush=True)
    return 0


if __name__ == "__main__":
    if sys.argv[1] == "--native":
        library = ctypes.CDLL(sys.argv[2])
        function = library.dispatch_probe
        function.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
        function.restype = ctypes.c_uint64
        for left in INPUTS:
            for right in INPUTS:
                assert function(left, right) == 66
    else:
        raise SystemExit(run(sys.argv[1], sys.argv[2]))
