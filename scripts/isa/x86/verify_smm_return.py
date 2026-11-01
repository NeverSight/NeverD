#!/usr/bin/env python3
"""Execute exported SMM source through NeverC/full LLVM and the native KVM oracle."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--compiler-source", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True)
    args = parser.parse_args()
    output = args.keep.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tools = {"neverc": args.compiler.resolve(),
             "llc": (args.llvm_bin / "llc").resolve(),
             "llvm-objdump": (args.llvm_bin / "llvm-objdump").resolve(),
             **{name: Path(shutil.which(name) or "")
                for name in ("clang", "ld.lld", "objcopy")}}
    for name, path in tools.items():
        if not path.is_file():
            parser.error(f"missing {name}: {path}")
    commands, profiles = [], []
    environment = dict(os.environ, TMPDIR=str(output))

    def run(command, name, expected=0):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=180, env=environment)
        (output / (name + ".stdout")).write_text(result.stdout)
        (output / (name + ".stderr")).write_text(result.stderr)
        commands.append({"command": command, "exit_code": result.returncode})
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}\n{result.stderr}")
        return result.stdout

    fixture = args.compiler_source / "tests/neverc/X86/smm_return_kvm.c"
    runner = output / "smm-oracle"
    run([tools["clang"], "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         fixture, "-o", runner], "oracle")
    linker = output / "guest.ld"
    linker.write_text("""ENTRY(generated_rsm_context_plain)
SECTIONS {
 . = 0x10000;
 .text : { *(.text*) }
 .rodata : { *(.rodata*) }
 .data : { *(.data*) }
 .bss : { *(.bss*) }
 /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
""")
    runtime = r'''
__attribute__((optnone,noinline)) void *memset(void *p,int c,size_t n) {
    for(size_t i=0;i<n;++i)((unsigned char *)p)[i]=(unsigned char)c;
    return p;
}
__attribute__((optnone,noinline)) void *memcpy(void *p,const void *q,size_t n) {
    for(size_t i=0;i<n;++i)((unsigned char *)p)[i]=((const unsigned char *)q)[i];
    return p;
}
static void prepare(const uint64_t *context) {
    neverd_x64_state_v12 *s=(void *)0x95000;
    const uint8_t *x=(const void *)(uintptr_t)context[0];
    memset(s,0,sizeof(*s));
    s->RFlags=2; s->PC=0x1000;
    s->CR2=context[2];s->CR8=context[3];s->KernelGSBase=context[4];
    s->SMM.XState=context[0]; memcpy(&s->MXCSR,x+24,4);
    const uint64_t bv=*(const uint64_t *)(x+512);
    for(unsigned i=0;i<16;++i) {
        if(bv&2)memcpy(s->ZMM[i],x+160+i*16,16);
        if(bv&4)memcpy(s->ZMM[i]+16,x+576+i*16,16);
    }
    // Force the shared projection to reconstruct SIMD from the state cells.
    memset((void *)(x+160),0xa5,256);memset((void *)(x+576),0x5a,256);
}
'''
    wrappers = ""
    for variant in ("plain", "fixed", "dynamic", "pressure"):
        frame = ""
        if variant == "pressure":
            frame = "volatile uint64_t local[32];\n"
            frame += "".join(f"local[{i}]={i+17};\n" for i in range(16))
            frame += "".join(f"uint64_t v{i}=local[{i}];\n" for i in range(16))
            frame += "".join(f"local[{i+16}]=v{i};\n" for i in range(16))
            frame += "context=(const uint64_t *)((const uint8_t *)context+("
            frame += "+".join(f"v{i}" for i in range(16)) + "-392));"
        elif variant != "plain":
            length = "size+32" if variant == "dynamic" else "512"
            frame = f"_Alignas(64) volatile unsigned char local[{length}]; local[0]=17; *marker=local[0];"
        wrappers += f'''
__attribute__((noinline)) void generated_rsm_context_{variant}(
    const uint64_t *context, uint64_t size, volatile unsigned *marker) {{
    {frame}
    *marker=17; prepare(context);
    neverd_step_v12_1000((void *)0x95000);
    __builtin_trap();
}}
'''
    for route in ("high", "llvm"):
        exported = args.sources / f"smm-{route}.c"
        source = output / f"smm-{route}-guest.c"
        source.write_text(exported.read_text() + runtime + wrappers)
        for level in ("O0", "O2"):
            stem = f"{route}-{level}"
            ir = output / (stem + ".ll")
            flags = ["--target=x86_64-unknown-linux-gnu", "-std=c23", "-" + level,
                     "-fno-lto", "-ffreestanding", "-fno-builtin-mimalloc",
                     "-fno-stack-protector", "-fno-pic", "-fno-pie", "-fno-inline",
                     "-mno-red-zone", "-mxsave", "-mavx"]
            run([tools["neverc"], *flags, "-S", "-emit-llvm", source, "-o", ir], stem + "-ir")
            if "@llvm.x86.rsm.context(" not in ir.read_text() or "asm " in ir.read_text():
                raise RuntimeError("SMM projection must use native context IR without asm")
            for backend in ("neverc", "llvm-legacy", "llvm-new"):
                name = stem + "-" + backend
                obj, elf, binary = (output / (name + ext) for ext in (".o", ".elf", ".bin"))
                if backend == "neverc":
                    run([tools["neverc"], *flags, "-c", source, "-o", obj], name + "-compile")
                else:
                    run([tools["llc"], "-mtriple=x86_64-unknown-linux-gnu", "-" + level,
                         "-mattr=+xsave,+avx", "-filetype=obj", "-verify-machineinstrs",
                         *(["-enable-new-pm"] if backend == "llvm-new" else []),
                         ir, "-o", obj], name + "-compile")
                run([tools["ld.lld"], "-m", "elf_x86_64", "-T", linker, obj, "-o", elf], name + "-link")
                run([tools["objcopy"], "-O", "binary", elf, binary], name + "-image")
                assembly = run([tools["llvm-objdump"], "-d", elf], name + "-assembly")
                terminals = re.findall(r"^\s*([0-9a-f]+):\s+0f aa\s+rsm\b", assembly, re.M)
                if len(terminals) != 1:
                    raise RuntimeError("expected one shared RSM boundary")
                instruction = int(terminals[0], 16)
                functions = re.findall(r"^([0-9a-f]+) <(generated_rsm_context_\w+)>:", assembly, re.M)
                if len(functions) != 4:
                    raise RuntimeError("missing compiler-frame wrappers")
                manifest = output / (name + ".manifest")
                manifest.write_text("".join(f"{fn} {address} {instruction:x} 4 64 0\n"
                                            for address, fn in functions))
                native = json.loads(run([runner, binary, manifest, "64", "projection"], name + "-run"))
                if native["complete_state_pairs"] != 3072:
                    raise RuntimeError("incomplete native SMM matrix")
                damaged = bytearray(binary.read_bytes())
                offset = instruction - 0x10000
                if damaged[offset-4:offset] != bytes.fromhex("49 0f ae 2b"):
                    raise RuntimeError("XRSTOR/RSM adjacency lost")
                damaged[offset-4:offset] = bytes.fromhex("90 90 90 90")
                negative = output / (name + "-missing-xrstor.bin")
                negative.write_bytes(damaged)
                run([runner, negative, manifest, "64", "projection"], name + "-negative", expected=1)
                if "actual_xstate" not in (output / (name + "-negative.stderr")).read_text():
                    raise RuntimeError("negative control did not reach the state oracle")
                profiles.append({"route": route, "optimization": level, "backend": backend,
                                 "source_sha256": digest(exported), "ir_sha256": digest(ir),
                                 "image_sha256": digest(binary), **native})
                print(name, native["complete_state_pairs"], flush=True)
    report = {"profiles": profiles,
              "complete_state_pairs": sum(p["complete_state_pairs"] for p in profiles),
              "fixture_sha256": digest(fixture),
              "fixture_header_sha256": digest(fixture.with_name("KvmTransferFixture.h")),
              "tools": {name: {"sha256": digest(path)} for name, path in tools.items()},
              "scope": "KVM-mediated native SMM, code64, XCR0=7; software state plus ambient SMI image"}
    (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
