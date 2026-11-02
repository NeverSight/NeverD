#!/usr/bin/env python3
"""Real call-site analysis: retained operands, per-call scope and native isolation."""
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from transport_test import Client
from source_synthesis_test import elf, ENTRY
from source_variable_types_test import ok, source

def fixture():
    return elf(bytes.fromhex("bf07000000 be0b000000 ba0d000000 e82c000000 bf11000000 e822000000 c3").ljust(64, b"\x90") + bytes.fromhex("89f8c3"))

def pe(code, machine=0x8664):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, machine, 1, 0, 0, 0, 0xF0, 0x22)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<I", data, optional + 4, 512)
    struct.pack_into("<I", data, optional + 16, 0x1000)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<II", data, optional + 32, 0x1000, 512)
    struct.pack_into("<II", data, optional + 56, 0x2000, 512)
    struct.pack_into("<H", data, optional + 68, 3)
    struct.pack_into("<I", data, optional + 108, 16)
    section = optional + 0xF0
    struct.pack_into("<8sIIIIIIHHI", data, section, b".text\0\0\0", 512, 0x1000, 512, 512, 0, 0, 0, 0, 0x60000020)
    data[512:512 + len(code)] = code
    return bytes(data)

def stack_fixture(partial=False):
    code = bytearray(bytes.fromhex("b907000000 ba0b000000 41b80d000000 41b911000000 4883ec38"))
    code += bytes.fromhex("66c74424201700" if partial else "48c744242017000000")
    offset = len(code)
    code += b"\xe8" + struct.pack("<i", 64 - offset - 5) + bytes.fromhex("4883c438 c3")
    return pe(bytes(code).ljust(64, b"\x90") + bytes.fromhex("89c8c3"))

def arm64_fixed_fixture():
    # STP/MOV frame; W0=7, D0=1.5, X1=11; BL +44; restore frame and RET.
    code = bytes.fromhex("fd7bbfa9 fd030091 e0008052 00106f1e 610180d2 0b000094 fd7bc1a8 c0035fd6")
    return pe(code.ljust(64, b"\0") + bytes.fromhex("c0035fd6"), 0xAA64)

def arm64_stack_fixture(partial=False):
    # Eight independent integer/FP register pairs, then a ninth pair on the
    # stack; consume the callee's floating result through FCVTZS X0,D0.
    code = bytearray(bytes.fromhex(
        "fd7bbfa9 fd030091 ff4300d1 20008052 00106e1e 41008052 0110601e "
        "62008052 0210611e 83008052 0310621e a4008052 0490621e c5008052 "
        "0510631e e6008052 0690631e 07018052 0710641e 28018052 e80300b9 "
        "1050641e f00700fd 29000094 0000789e ff430091 fd7bc1a8 c0035fd6"))
    if partial:
        code[80:84] = bytes.fromhex("e8030079")  # STRH supplies only half of uint32_t.
    return pe(bytes(code).ljust(256, b"\0") + bytes.fromhex("00106e1e c0035fd6"), 0xAA64)

def arm64_variadic_fixture(partial=False):
    # Windows ARM64: named float/int and six unnamed scalars occupy X0-X7;
    # the final double/integer pair is at SP/SP+8. The double result uses D0.
    code = bytearray(bytes.fromhex(
        "fd7bbfa9 fd030091 ff4300d1 00f8a752 e1008052 8200e8d2 630180d2 "
        "8401f8d2 a50180d2 4602e8d2 270280d2 c802e8d2 e80300f9 "
        "e80280d2 e80700f9 11000094 0000789e ff430091 fd7bc1a8 c0035fd6"))
    if partial:
        code[48:52] = bytes.fromhex("e80300b9")  # STRW cannot supply a double.
    return pe(bytes(code).ljust(128, b"\0") + bytes.fromhex("00106e1e c0035fd6"), 0xAA64)

def request(view, call, declaration, actuals=()):
    return {"address": view["address"], "representation": view["representation"], "kind": "call_type",
        "call_id": call["id"], "snapshot": view["call_snapshot"], "declaration": declaration,
        "variadic_types": list(actuals)}

# The engine's C is Clang's dialect: it writes an exact floating-point
# constant with __builtin_bit_cast, and calls a declared function through a
# cast to its call site's own type, such as an ms_abi variadic one. GCC lacks
# the builtin and folds such a cast into a call with the declared ABI, so its
# runs would not test this output. Without Clang they are unexecuted
# coverage, never a pass.
UNEXECUTED = []
_clang = {}

def is_clang(root, compiler):
    if compiler not in _clang:
        probe = root / "clang-dialect.c"
        probe.write_text("#ifndef __clang__\n#error not Clang\n#endif\nint probe;\n")
        if Path(compiler).stem.lower() in ("cl", "clang-cl"):
            command = [compiler, "/nologo", "/TC", "/c", str(probe), "/Fo:" + str(root / "clang-dialect.obj")]
        else:
            command = [compiler, "-c", str(probe), "-o", str(root / "clang-dialect.o")]
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
        _clang[compiler] = result.returncode == 0
    return _clang[compiler]

def execute_source(root, text, route, compiler, harness=None, optimization=None):
    if not is_clang(root, compiler):
        UNEXECUTED.append(route)
        return
    c_file, harness_file, program = root / (route + ".c"), root / (route + "-harness.c"), root / (route + ".exe")
    c_file.write_text(text)
    harness_file.write_text(harness or """
#include <stdint.h>
#include <stdarg.h>
static int calls, errors;
extern int64_t start(void);
uint64_t sub_4000B8(uint32_t first, ...) {
  ++calls;
  if (first == 7) {
    va_list ap; va_start(ap, first);
    unsigned a = va_arg(ap, unsigned), b = va_arg(ap, unsigned);
    errors += a != 11 || b != 13; va_end(ap);
  }
  return first;
}
int main(void) { return start() != 17 || calls != 2 || errors; }
""")
    if Path(compiler).stem.lower() in ("cl", "clang-cl"):
        command = [compiler, "/nologo", "/TC", "/std:c11", str(c_file), str(harness_file), "/Fe:" + str(program)]
    else:
        command = [compiler, "-std=c11", "-fno-strict-aliasing", str(c_file), str(harness_file), "-o", str(program)]
    if optimization is not None:
        command.append(("/Od" if optimization == "O0" else "/O2") if Path(compiler).stem.lower() in ("cl", "clang-cl") else "-" + optimization)
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([str(program)], cwd=root, check=True, timeout=10)

def run(executable, compiler=None):
    with tempfile.TemporaryDirectory(prefix="neverd-call-types-") as directory:
        root = Path(directory)
        binary = root / "calls.elf"
        binary.write_bytes(fixture())
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary), "analysis": False})
            before = {route: source(client, route) for route in ("c", "llvmc")}
            native = {route: source(client, route)["text"] for route in ("low", "med", "llvm")}
            call = before["c"]["source_calls"][0]
            edit = request(before["c"], call, "uint64_t call(uint32_t first, ...);", ("unsigned int", "unsigned int"))
            for invalid in ({**edit, "snapshot": "obsolete"}, {**edit, "variadic_types": ["char"]},
                            {**edit, "declaration": "struct MISSING call(int x);"}):
                state = ok(client, "analysis_snapshot")
                refused = client.call("code_edit", invalid)
                assert refused["status"] == "error", refused
                assert ok(client, "analysis_snapshot") == state
            ok(client, "code_edit", edit)
            after = {route: source(client, route) for route in ("c", "llvmc")}
            for route, view in after.items():
                assert view["call_type_results"] == [{"id": call["id"], "applied": True, "refusal": ""}], view
                assert "(*)(uint32_t, ...)" in view["text"] and "0xB" in view["text"] and "0xD" in view["text"], view
                assert "objc/" not in view["text"], view
                # The original sibling call to this target keeps its argument list.
                sibling = next(line for line in before[route]["text"].splitlines() if "return " in line)
                assert sibling in view["text"], (sibling, view["text"])
                declarations = [line for line in before[route]["text"].splitlines() if line and not line[0].isspace() and "sub_4000B8(" in line and line.endswith(";")]
                assert all(line in view["text"].splitlines() for line in declarations), (declarations, view["text"])
                if compiler: execute_source(root, view["text"], route, compiler)
            for route, text in native.items(): assert source(client, route)["text"] == text
            ok(client, "undo")
            for route in before: assert source(client, route)["text"] == before[route]["text"]
            ok(client, "redo")
            for route in after: assert source(client, route)["text"] == after[route]["text"]
            with Client(executable) as replica:
                ok(replica, "analysis_restore", ok(client, "analysis_snapshot"))
                assert source(replica)["text"] == after["c"]["text"]
                refused = replica.call("code_edit", {**edit, "reset": True})
                assert refused["status"] == "error" and refused["error"]["code"] == "read_only", refused
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary), "analysis": False})
            for route in after: assert source(client, route)["text"] == after[route]["text"]
            ok(client, "code_edit", {**edit, "reset": True})
            for route in before: assert source(client, route)["text"] == before[route]["text"]
            # Fixed prototypes use the same route and can change the arg count
            # before SSA DCE, retaining actual machine writes of RSI and RDX.
            fixed = {**edit, "declaration": "uint64_t call(uint32_t a, uint32_t b, uint32_t c);", "variadic_types": []}
            ok(client, "code_edit", fixed)
            assert "(*)(uint32_t, uint32_t, uint32_t)" in source(client)["text"]
            ok(client, "code_edit", {**fixed, "action": "clear"})
            assert source(client)["text"] == before["c"]["text"]
            ok(client, "code_edit", {**edit, "declaration": "void call(uint32_t a, uint32_t b, uint32_t c);", "variadic_types": []})
            for route in ("c", "llvmc"):
                result = source(client, route)
                assert result["call_type_results"][0]["applied"] and "void (*)(uint32_t, uint32_t, uint32_t)" in result["text"], result
        # Indirect targets retain the original pointer rather than resolving the
        # analyst's placeholder declaration name as a global symbol.
        indirect = root / "indirect.elf"
        indirect.write_bytes(elf(bytes.fromhex("4889f8 bf07000000 be0b000000 ba0d000000 ffd0 c3")))
        with Client(executable) as client:
            ok(client, "open", {"path": str(indirect), "analysis": False})
            view = source(client)
            call = view["source_calls"][0]
            assert call["indirect"], call
            ok(client, "code_edit", request(view, call, "uint64_t call(uint32_t a, uint32_t b, uint32_t c);"))
            for route in ("c", "llvmc"):
                result = source(client, route)
                assert result["call_type_results"][0]["applied"] and "(*)(uint32_t, uint32_t, uint32_t)" in result["text"] and "0xD" in result["text"], result
                assert "call(" not in result["text"], result
        # Opaque LLVM pointers must retain the declared record pointee in C.
        records = root / "records.elf"
        records.write_bytes(elf(bytes.fromhex("53 e80f000000 4889c3 8b4304 034308 5b c3 90909090 4889f8 c3")))
        with Client(executable) as client:
            ok(client, "open", {"path": str(records), "analysis": False})
            ok(client, "local_types_declare", {"source": "struct QA_CONTEXT { unsigned int State; unsigned int Length; unsigned int Flags; };"})
            view = source(client)
            ok(client, "code_edit", request(view, view["source_calls"][0], "struct QA_CONTEXT *call(struct QA_CONTEXT *context);"))
            for route in ("c", "llvmc"):
                result = source(client, route)
                assert result["call_type_results"][0]["applied"] and "QA_CONTEXT*(*)(QA_CONTEXT*)" in "".join(result["text"].split()), result["text"]
                if route == "c": assert "->Length" in result["text"] and "->Flags" in result["text"], result["text"]
                if compiler:
                    execute_source(root, result["text"], "record-" + route, compiler, """
#include <stdint.h>
struct QA_CONTEXT { unsigned State, Length, Flags; };
struct QA_CONTEXT *sub_40008D(struct QA_CONTEXT *context) { return context; }
extern int32_t start(uintptr_t context);
int main(void) { struct QA_CONTEXT context = { 1, 7, 9 }; return start((uintptr_t)&context) != 16; }
""")
        for count in (0, 1, 2, 3):
            floating = root / ("floating-%d.elf" % count)
            code = bytes.fromhex("48b8000000000000f83f 66480f6ec0 bf07000000")
            # The third caller computes the exact AL count as 3 - 2. A COPY-only
            # proof loses that value even though the shared scalar owner knows it.
            if count == 3:
                # Incoming R11 is unknown, but EAX-EAX followed by INC is one.
                code += bytes.fromhex("4c89d8 29c0 ffc0")
            elif count == 2:
                code += bytes.fromhex("b803000000 83e802")
            else:
                code += b"\xb8" + struct.pack("<I", count)
            code += b"\xe8" + struct.pack("<i", 64 - len(code) - 5) + b"\xc3"
            floating.write_bytes(elf(code.ljust(64, b"\x90") + bytes.fromhex("89f8c3")))
            with Client(executable) as client:
                ok(client, "open", {"path": str(floating), "analysis": False})
                view = source(client)
                edit = request(view, view["source_calls"][0], "uint64_t call(uint32_t first, ...);", ["double"])
                reply = client.call("code_edit", edit)
                if not count:
                    assert reply["status"] == "error" and "AL vector-register count" in reply["error"]["message"], reply
                    continue
                assert reply["status"] == "ok", reply
                for route in ("c", "llvmc"):
                    result = source(client, route)
                    assert result["call_type_results"][0]["applied"], result
                    if compiler:
                        for optimization in ("O0", "O2"):
                            execute_source(root, result["text"], "floating-%d-%s-%s" % (count, route, optimization), compiler, """
#include <stdint.h>
#include <stdarg.h>
extern uint64_t start(void);
static int errors;
uint64_t sub_4000B8(uint32_t first, ...) {
  va_list ap; va_start(ap, first); errors += va_arg(ap, double) != 1.5; va_end(ap); return first;
}
int main(void) { return start() != 7 || errors; }
""", optimization=optimization)
        arm64 = root / "arm64-fixed.exe"
        arm64.write_bytes(arm64_fixed_fixture())
        with Client(executable) as client:
            ok(client, "open", {"path": str(arm64), "analysis": False})
            view = source(client, "c", 0x140001000)
            edit = request(view, view["source_calls"][0], "uint64_t call(uint32_t first, double value, uintptr_t cookie);")
            ok(client, "code_edit", edit)
            for route in ("c", "llvmc"):
                result = source(client, route, 0x140001000)
                assert result["call_type_results"][0]["applied"] and "double" in result["text"], result
                if compiler:
                    for optimization in ("O0", "O2"):
                        execute_source(root, result["text"], "arm64-fixed-" + route + "-" + optimization, compiler, """
#include <stdint.h>
extern uint64_t start(void);
static unsigned calls, errors;
uint64_t sub_140001040(uint32_t first, double value, uintptr_t cookie) {
  ++calls; errors += first != 7 || value != 1.5 || cookie != 11; return first;
}
int main(void) { return start() != 7 || calls != 1 || errors; }
""", optimization)
            baseline = ok(client, "analysis_snapshot")
            view = source(client, "c", 0x140001000)
            refused = client.call("code_edit", request(view, view["source_calls"][0], "uint64_t call(uint32_t first, ...);", ["float", "uintptr_t"]))
            assert refused["status"] == "error" and "default promotions" in refused["error"]["message"], refused
            assert ok(client, "analysis_snapshot") == baseline
        for partial in (False, True):
            arm64_varargs = root / ("arm64-varargs-partial.exe" if partial else "arm64-varargs.exe")
            arm64_varargs.write_bytes(arm64_variadic_fixture(partial))
            with Client(executable) as client:
                ok(client, "open", {"path": str(arm64_varargs), "analysis": False})
                view = source(client, "c", 0x140001000)
                baseline = ok(client, "analysis_snapshot")
                edit = request(view, view["source_calls"][0], "double call(float scale, uint32_t first, ...);", ["double", "uint64_t"] * 4)
                reply = client.call("code_edit", edit)
                if partial:
                    assert reply["status"] == "error" and "fully written" in reply["error"]["message"], reply
                    assert ok(client, "analysis_snapshot") == baseline
                    continue
                assert reply["status"] == "ok", reply
                for route in ("c", "llvmc"):
                    result = source(client, route, 0x140001000)
                    assert result["call_type_results"][0]["applied"], result
                    if compiler:
                        for optimization in ("O0", "O2"):
                            harness = """
#include <stdint.h>
#include <stdarg.h>
extern START_RESULT start(void);
static unsigned calls, errors;
double sub_140001080(float scale, uint32_t first, ...) {
  const double floating[] = { 2.5, -3.5, 4.5, 5.5 };
  const uint64_t integers[] = { 11, 13, 17, 23 };
  va_list ap; va_start(ap, first);
  ++calls; errors += scale != 1.5f || first != 7;
  for (unsigned i = 0; i < 4; ++i) {
    errors += va_arg(ap, double) != floating[i];
    errors += va_arg(ap, uint64_t) != integers[i];
  }
  va_end(ap); return 42.75;
}
int main(void) { return start() != 42 || calls != 1 || errors; }
""".replace("START_RESULT", "int64_t" if route == "c" else "uint64_t")
                            execute_source(root, result["text"], "arm64-varargs-" + route + "-" + optimization, compiler, harness, optimization)
        for partial in (False, True):
            arm64_stack = root / ("arm64-stack-partial.exe" if partial else "arm64-stack.exe")
            arm64_stack.write_bytes(arm64_stack_fixture(partial))
            parameters = ", ".join("uint32_t i%d, double f%d" % (i, i) for i in range(9))
            with Client(executable) as client:
                ok(client, "open", {"path": str(arm64_stack), "analysis": False})
                view = source(client, "c", 0x140001000)
                baseline = ok(client, "analysis_snapshot")
                edit = request(view, view["source_calls"][0], "double call(%s);" % parameters)
                reply = client.call("code_edit", edit)
                if partial:
                    assert reply["status"] == "error" and "fully written" in reply["error"]["message"], reply
                    assert ok(client, "analysis_snapshot") == baseline
                    continue
                assert reply["status"] == "ok", reply
                for route in ("c", "llvmc"):
                    result = source(client, route, 0x140001000)
                    assert result["call_type_results"][0]["applied"], result
                    if compiler:
                        for optimization in ("O0", "O2"):
                            harness = """
#include <stdint.h>
extern START_RESULT start(void);
static unsigned calls, errors;
double sub_140001100(PARAMETERS) {
  const unsigned integers[] = { i0, i1, i2, i3, i4, i5, i6, i7, i8 };
  const double floating[] = { f0, f1, f2, f3, f4, f5, f6, f7, f8 };
  ++calls;
  for (unsigned i = 0; i < 9; ++i) {
    errors += integers[i] != i + 1;
    errors += floating[i] != i + 1;
  }
  return 42.75;
}
int main(void) { return start() != 42 || calls != 1 || errors; }
""".replace("PARAMETERS", parameters).replace("START_RESULT", "int64_t" if route == "c" else "uint64_t")
                            execute_source(root, result["text"], "arm64-stack-" + route + "-" + optimization, compiler, harness, optimization)
        # Real PE / Win64 call: its fifth value is read from the written stack
        # bytes, while a partial write cannot manufacture the missing bytes.
        for partial in (False, True):
            path = root / ("partial.exe" if partial else "stack.exe")
            path.write_bytes(stack_fixture(partial))
            with Client(executable) as client:
                ok(client, "open", {"path": str(path), "analysis": False})
                view = source(client, "c", 0x140001000)
                edit = request(view, view["source_calls"][0], "uint64_t call(uint32_t first, ...);", ["unsigned int"] * 4)
                reply = client.call("code_edit", edit)
                if partial:
                    assert reply["status"] == "error" and "fully written" in reply["error"]["message"], reply
                else:
                    assert reply["status"] == "ok", reply
                    for route in ("c", "llvmc"):
                        result = source(client, route, 0x140001000)
                        assert result["call_type_results"][0]["applied"] and "0x17" in result["text"], result
                        if compiler:
                            execute_source(root, result["text"], "win64-" + route, compiler, """
#include <stdint.h>
#include <stdarg.h>
#if MS_ABI_CALL && !defined(_WIN32) && (defined(__x86_64__) || defined(__aarch64__))
#define CALLEE_ABI __attribute__((ms_abi))
#define va_list __builtin_ms_va_list
#undef va_start
#define va_start(ap, last) __builtin_ms_va_start(ap, last)
#undef va_end
#define va_end(ap) __builtin_ms_va_end(ap)
#else
#define CALLEE_ABI
#endif
extern START_RESULT start(void);
static unsigned calls, errors;
uint64_t CALLEE_ABI sub_140001040(uint32_t first, ...) {
  const unsigned expected[] = { 11, 13, 17, 23 };
  va_list ap; va_start(ap, first);
  ++calls; errors += first != 7;
  for (unsigned i = 0; i != 4; ++i)
    errors += va_arg(ap, unsigned) != expected[i];
  va_end(ap); return first;
}
int main(void) { return start() != 7 || calls != 1 || errors; }
""".replace("START_RESULT", "int64_t" if route == "c" else "uint64_t")
                               .replace("MS_ABI_CALL", "0" if route == "c" else "1"))
                    # A complete declaration also fixes the operand count.
                    # Other registers and stack bytes set at this call do
                    # not extend a shorter, explicitly declared signature.
                    for count in (0, 1, 2, 4, 5):
                        view = source(client, "c", 0x140001000)
                        parameters = ", ".join("uint32_t a%d" % index for index in range(count)) or "void"
                        declaration = "uint64_t call(%s);" % parameters
                        ok(client, "code_edit", request(view, view["source_calls"][0], declaration))
                        for route in ("c", "llvmc"):
                            result = source(client, route, 0x140001000)
                            assert result["call_type_results"][0]["applied"], result
    if UNEXECUTED:
        print("SKIP: %s is not Clang, whose dialect the engine's C uses; "
              "not executed: %s" % (compiler, ", ".join(UNEXECUTED)))
        return 77

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc")
    arguments = parser.parse_args()
    sys.exit(run(arguments.worker, arguments.cc))
