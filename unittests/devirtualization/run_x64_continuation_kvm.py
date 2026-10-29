#!/usr/bin/env python3
"""Compile a generated continuation fixture and execute it on native x64 KVM."""

import argparse
import pathlib
import platform
import subprocess
import sys

GUEST_TIMEOUT_SECONDS = 15
BOOTSTRAP = r"""
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *a=d; const unsigned char *b=s;
  for(size_t i=0;i<n;++i) a[i]=b[i]; return d;
}
void *memset(void *d, int c, size_t n) {
  unsigned char *a=d; for(size_t i=0;i<n;++i) a[i]=c; return d;
}
int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x=a,*y=b;
  for(size_t i=0;i<n;++i) if(x[i]!=y[i]) return x[i]<y[i]?-1:1;
  return 0;
}
__attribute__((noreturn)) void abort(void) {
  __asm__ volatile("mov $1, %%eax; hlt" ::: "rax", "memory");
  __builtin_unreachable();
}
__attribute__((naked,section(".start"))) void _start(void) {
  __asm__ volatile("call main; hlt");
}
"""
LINKER_SCRIPT = """
ENTRY(_start)
SECTIONS {
  . = 0x10000;
  .text : { *(.start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--optimization", choices=("O0", "O2"), required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine() not in ("x86_64", "amd64"):
        print("native x64 KVM requires a Linux x64 host", file=sys.stderr)
        return 77
    stem = args.source.with_suffix("." + args.optimization)
    guest = stem.with_suffix(stem.suffix + ".guest.c")
    guest.write_text(args.source.read_text() + BOOTSTRAP)
    linker = stem.with_suffix(stem.suffix + ".ld")
    linker.write_text(LINKER_SCRIPT)
    binary = stem.with_suffix(stem.suffix + ".bin")
    runner = stem.with_suffix(stem.suffix + ".runner")
    subprocess.run(
        [args.clang, "-std=gnu11", "-O2", str(pathlib.Path(__file__).parent /
         "fixtures" / "x64_continuation_kvm.c"), "-o", str(runner)], check=True
    )
    subprocess.run(
        [args.clang, "-std=gnu11", "-" + args.optimization, "-ffreestanding",
         "-fno-builtin", "-mgeneral-regs-only", "-mno-red-zone",
         "-fno-stack-protector", "-fno-pic", "-fno-pie", "-nostdlib", "-no-pie",
         "-Wl,--build-id=none", "-Wl,--oformat=binary", "-Wl,-T," + str(linker),
         str(guest), "-o", str(binary)], check=True
    )
    return subprocess.run([str(runner), str(binary)],
                          timeout=GUEST_TIMEOUT_SECONDS).returncode


if __name__ == "__main__":
    sys.exit(main())
