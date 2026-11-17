#!/usr/bin/env python3
"""Virtual tail transfers preserve untouched integer and floating carriers."""
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload):
    response = client.call(operation, payload, timeout=120)
    assert response["status"] == "ok", response
    return response["payload"]


def run(worker, target):
    clang, linker = shutil.which("clang"), shutil.which("ld.lld")
    if not clang or not linker:
        print("SKIP: requires clang and ld.lld")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-virtual-tail-") as directory:
        root = Path(directory)
        source, obj, image = (root / name for name in ("input.cpp", "input.o", "input.so"))
        source.write_text("""
struct Device {
  virtual void update(unsigned long long value, float delta, double weight);
  virtual void record(unsigned long long value, void *context);
  virtual int transform(float ratio, unsigned code);
  virtual double many(float a0, float a1, float a2, float a3, float a4,
                      float a5, float a6, float a7, float a8, unsigned long long marker);
};
void Device::update(unsigned long long, float, double) {}
void Device::record(unsigned long long, void *) {}
int Device::transform(float, unsigned) { return -1; }
double Device::many(float, float, float, float, float, float, float, float,
                    float, unsigned long long) { return 0; }
extern "C" void forward_tail(Device *object, unsigned long long value,
                             float delta, double weight) {
  object->update(value, delta, weight);
}
extern "C" void forward_integer(Device *object, unsigned long long value,
                                void *context) {
  object->record(value, context);
}
extern "C" int forward_result(Device *object, float ratio, unsigned code) {
  return object->transform(ratio, code);
}
extern "C" double forward_many(Device *object, float a0, float a1, float a2,
                               float a3, float a4, float a5, float a6, float a7,
                               float a8, unsigned long long marker) {
  return object->many(a0, a1, a2, a3, a4, a5, a6, a7, a8, marker);
}
""")
        subprocess.run([clang, "--target=" + target, "-g", "-gdwarf-4", "-O2",
                        "-fPIC", "-fno-rtti", "-fno-exceptions", "-c", str(source),
                        "-o", str(obj)], check=True)
        subprocess.run([linker, "-shared", "-Bsymbolic", "--no-undefined",
                        "-z", "common-page-size=65536", "-z", "max-page-size=65536",
                        str(obj), "-o", str(image)], check=True)
        with Client(worker) as client:
            ok(client, "open", {"path": str(image), "analysis": False, "warm": False})
            views = {}
            for name in ("forward_tail", "forward_integer", "forward_result", "forward_many"):
                address = ok(client, "resolve", {"query": name})["address"]
                for representation in ("c", "llvmc"):
                    view = ok(client, "decompile", {
                        "address": address, "representation": representation, "limit": 2048})
                    views[name, representation] = view
                    assert view["complete"], view
                    assert "__builtin_trap" not in view["text"], view["text"]
                    if representation == "c":
                        assert len(view["virtual_calls"]) == 1, view
            native = (platform.machine() in {"arm64", "aarch64"}
                      and target.startswith("aarch64")) or (
                          platform.machine() in {"x86_64", "AMD64"}
                          and target.startswith("x86_64"))
            for (name, representation), view in views.items():
                code = view["text"]
                call_lines = [line for line in code.splitlines() if "/* virtual " in line]
                if representation == "c":
                    assert len(call_lines) == 1, code
                expected = {"forward_tail": ("value", "delta", "weight"),
                            "forward_integer": ("value", "context"),
                            "forward_result": ("ratio", "code"),
                            "forward_many": ("a0", "marker")}[name]
                if representation == "c":
                    assert all(item in call_lines[0] for item in expected), call_lines
                    if name == "forward_many":
                        # A writable incoming home may carry a8 through a
                        # temporary. Require its use beyond the signature;
                        # the independent caller below checks the actual value.
                        definition = re.search(r"\bforward_many\([^;{]*\)\s*\{", code)
                        assert definition and re.search(r"\ba8\b", code[definition.end():]), code
                if not native:
                    continue
                harness = r"""
#include <stdint.h>
typedef struct Device Device;
extern void forward_tail(Device*, uint64_t, float, double);
extern void forward_integer(Device*, uint64_t, void*);
extern int forward_result(Device*, float, unsigned);
extern double forward_many(Device*, float, float, float, float, float,
                           float, float, float, float, uint64_t);
static int observed;
static void actual(Device *object, uint64_t value, float delta, double weight) {
  observed = object != 0 && value == UINT64_C(0xFEDCBA9876543210)
             && delta == 3.25f && weight == -6.5;
}
static void record_actual(Device *object, uint64_t value, void *context) {
  observed = object != 0 && value == UINT64_C(0xFEDCBA9876543210)
             && context == &observed;
}
static int transform_actual(Device *object, float ratio, unsigned code) {
  return object != 0 && ratio == -2.25f && code == UINT32_C(0xFEDCBA98)
         ? -1234567 : 111;
}
static double many_actual(Device *object, float a0, float a1, float a2,
                          float a3, float a4, float a5, float a6, float a7,
                          float a8, uint64_t marker) {
  return object && a0 == 1.25f && a1 == 2.25f && a2 == 3.25f && a3 == 4.25f
         && a4 == 5.25f && a5 == 6.25f && a6 == 7.25f && a7 == 8.25f
         && a8 == 9.25f && marker == UINT64_C(0xFEDCBA9876543210) ? 23.5 : -1;
}
int main(void) {
  void *table[] = {(void *)&actual, (void *)&record_actual,
                   (void *)&transform_actual, (void *)&many_actual};
  void *object = table;
  INVOKE;
  return !observed;
}
"""
                invocation = ("forward_tail((Device*)&object, UINT64_C(0xFEDCBA9876543210), 3.25f, -6.5)"
                              if name == "forward_tail" else
                              "forward_integer((Device*)&object, UINT64_C(0xFEDCBA9876543210), &observed)"
                              if name == "forward_integer" else
                              "observed = forward_result((Device*)&object, -2.25f, UINT32_C(0xFEDCBA98)) == -1234567"
                              if name == "forward_result" else
                              "observed = forward_many"
                              "((Device*)&object, 1.25f, 2.25f, 3.25f, 4.25f, 5.25f, 6.25f, 7.25f, 8.25f, 9.25f, UINT64_C(0xFEDCBA9876543210)) == 23.5")
                emitted = root / (name + "-" + representation + ".c")
                emitted.write_text(code)
                # The independent caller uses the original native ABI. Separate
                # translation units keep the optimizer from treating a changed
                # source parameter order as an incompatible C function cast.
                oracle = root / "oracle.c"
                oracle.write_text(harness.replace("INVOKE", invocation))
                for level in ("-O0", "-O2"):
                    executable = root / "check"
                    try:
                        subprocess.run([clang, "-std=gnu11", "-fno-strict-aliasing", "-fno-lto", level,
                                        str(emitted), str(oracle), "-o", str(executable)], check=True,
                                       capture_output=True)
                        subprocess.run([str(executable)], check=True, capture_output=True)
                    except subprocess.CalledProcessError as error:
                        raise AssertionError((name, representation, level, code, error.stderr, client.logs)) from error
            print(target, "PASS", "native HighC/LLVMC O0/O2" if native else "native execution unavailable")
    return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1], sys.argv[2] if len(sys.argv) > 2
                         else "aarch64-unknown-linux-gnu"))
