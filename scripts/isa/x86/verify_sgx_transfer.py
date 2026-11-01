#!/usr/bin/env python3
"""Compare exported SGX input state at the immutable original ENCLU boundary."""

import argparse
import hashlib
import importlib.util
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
    fixture_root = args.compiler_source / "tests/neverc/X86"
    spec = importlib.util.spec_from_file_location("frames", fixture_root / "verify_frame_returns.py")
    frames = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(frames)
    tools = {"neverc": args.compiler.resolve(),
             **{name: (args.llvm_bin / name).resolve() for name in ("llc", "opt", "llvm-objdump")},
             **{name: Path(shutil.which(name) or "") for name in ("clang", "ld.lld", "objcopy")}}
    for name, path in tools.items():
        if not path.is_file():
            parser.error(f"missing {name}: {path}")
    tool_hashes = {name: digest(path) for name, path in tools.items()}
    commands, profiles = [], []
    environment = dict(os.environ, TMPDIR=str(output))

    def run(command, name, expected=0):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True, timeout=240, env=environment)
        (output / (name + ".stdout")).write_text(result.stdout)
        (output / (name + ".stderr")).write_text(result.stderr)
        commands.append(dict(command=command, exit_code=result.returncode))
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}\n{result.stderr}")
        return result.stdout

    fixture = fixture_root / "sgx_transfer_context_kvm.c"
    runner = output / "sgx-input-oracle"
    run([tools["clang"], "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", fixture, "-o", runner], "oracle")
    linker = output / "guest.ld"
    linker.write_text("""ENTRY(generated_enclu_transfer_context_plain)
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
static void prepare(const void *opaque) {
    const uint64_t *context=opaque;
    neverd_x64_state_v13 *s=(void *)0x95000;
    memset(s,0,sizeof(*s));
    memcpy(s->GPR,context,sizeof(s->GPR));
    s->RFlags=context[16];s->PC=0x1000;s->SGX.XState=context[17];
    s->CR2=0x1234567800010000ULL;
    if((uint32_t)s->GPR[0]-2u>2u)return;
    const uint8_t *x=(const void *)(uintptr_t)context[17];
    memcpy(&s->MXCSR,x+24,4);
    const uint64_t bv=*(const uint64_t *)(x+512);
    for(unsigned i=0;i<16;++i) {
        if(bv&2)memcpy(s->ZMM[i],x+160+i*16,16);
        if(bv&4)memcpy(s->ZMM[i]+16,x+576+i*16,16);
    }
    // SIMD must be reconstructed from explicit state, not this poisoned image.
    memset((void *)(x+160),0xa5,256);memset((void *)(x+576),0x5a,256);
}
'''
    _, wrappers = frames.sources(False, {"enclu_transfer_context": (0, 64, 0, "enclu")})
    wrappers = wrappers.replace(
        "__builtin_ia32_enclu_transfer_context(destination);",
        "prepare(destination);\n  *marker=(unsigned)neverd_step_v13_1000((void *)0x95000);\n  __builtin_trap();")
    flags = ["--target=x86_64-unknown-linux-gnu", "-std=c23", "-fno-lto", "-ffreestanding",
             "-fno-builtin-mimalloc", "-fno-pic", "-fno-pie", "-fno-stack-protector",
             "-mno-red-zone", "-msgx", "-mxsave", "-mavx"]
    for route in ("high", "llvm"):
        source = args.sources / f"sgx-{route}.c"
        binding = json.loads((args.sources / f"sgx-{route}.json").read_text())
        assert binding["state_version"] == 13 and binding["state_bytes"] == 2464
        assert binding["native_requests"] == 0
        assert binding["source_sha256"].lower() == digest(source)
        combined = output / f"{route}.c"
        combined.write_text(source.read_text() + runtime + wrappers)
        for backend in ("neverc", "llvm-legacy", "llvm-new"):
            for level in ("O0", "O2"):
                name = f"{route}-{backend}-{level}"
                obj = output / (name + ".o")
                if backend == "neverc":
                    run([tools["neverc"], *flags, "-" + level, "-c", combined, "-o", obj], name + "-compile")
                else:
                    ir = output / (name + ".ll")
                    run([tools["neverc"], *flags, "-" + level, "-S", "-emit-llvm", combined, "-o", ir], name + "-ir")
                    assert "@llvm.x86.enclu.transfer.at.context(" in ir.read_text() and "asm " not in ir.read_text()
                    if level == "O2":
                        optimized = output / (name + "-optimized.ll")
                        run([tools["opt"], "-S", "-passes=default<O2>", ir, "-o", optimized], name + "-opt")
                        ir = optimized
                    run([tools["llc"], "-mtriple=x86_64-unknown-linux-gnu", "-" + level,
                         "-filetype=obj", "-verify-machineinstrs",
                         *(["-enable-new-pm"] if backend == "llvm-new" else []), ir, "-o", obj], name + "-compile")
                elf, binary = output / (name + ".elf"), output / (name + ".bin")
                run([tools["ld.lld"], "-m", "elf_x86_64", "-T", linker, obj, "-o", elf], name + "-link")
                run([tools["objcopy"], "-O", "binary", elf, binary], name + "-image")
                assembly = run([tools["llvm-objdump"], "-d", elf], name + "-assembly")
                bodies = re.findall(r"^([0-9a-f]+) <(generated_enclu_transfer_context_\w+)>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)", assembly, re.M | re.S)
                assert len(bodies) == 4, assembly
                entries = []
                for address, function, body in bodies:
                    rejection = re.findall(r"^\s*([0-9a-f]+):\s+0f 0b\s+ud2", body, re.M)
                    assert rejection, body
                    entries.append(f"{function} {address} 1000 {rejection[-1]}\n")
                manifest = output / (name + ".manifest")
                manifest.write_text("".join(entries))
                native = json.loads(run([runner, binary, manifest, "64", "projected"], name + "-run"))
                assert native["input_state_observations"] == 9216 and native["invalid_leaves_rejected"] == 24
                assert native["enclave_instructions_executed"] == 0
                data = binary.read_bytes()
                damaged = bytearray(data)
                restores = re.findall(r"^\s*([0-9a-f]+):\s+48 0f ae 2e\s+xrstor64", assembly, re.M)
                assert restores, assembly
                for pc in restores:
                    offset = int(pc, 16) - 0x10000
                    assert damaged[offset:offset+4] == bytes.fromhex("48 0f ae 2e")
                    damaged[offset:offset+4] = b"\x90" * 4
                negative = output / (name + "-missing-xrstor.bin")
                negative.write_bytes(damaged)
                run([runner, negative, manifest, "64", "projected"], name + "-negative", expected=1)
                assert "actual_xstate" in (output / (name + "-negative.stderr")).read_text()
                jumps = re.findall(r"^\s*([0-9a-f]+):\s+ff 25 (?:[0-9a-f]{2} ){3}[0-9a-f]{2}\s+jmpq?\s+\*", assembly, re.M)
                damaged = bytearray(data)
                originals = 0
                for pc in jumps:
                    jump = int(pc, 16) - 0x10000
                    literal = jump + 6 + int.from_bytes(data[jump+2:jump+6], "little", signed=True)
                    if int.from_bytes(data[literal:literal+8], "little") != 0x1000:
                        continue
                    assert (literal + 0x10000) % 8 == 0, "unaligned original-site target"
                    damaged[literal:literal+8] = (0x1001).to_bytes(8, "little")
                    originals += 1
                assert originals > 0, "missing original-site jump"
                negative = output / (name + "-wrong-site.bin")
                negative.write_bytes(damaged)
                run([runner, negative, manifest, "64", "projected"], name + "-wrong-site", expected=1)
                assert "expected 0x1000" in (output / (name + "-wrong-site.stderr")).read_text()
                profiles.append(dict(route=route, backend=backend, optimization=level,
                                     source_sha256=digest(source), image_sha256=digest(binary), **native))
                print(name, native["input_state_observations"], flush=True)
    assert tool_hashes == {name: digest(tools[name]) for name in tool_hashes}, "compiler changed during qualification"
    report = dict(profiles=profiles, input_state_observations=sum(p["input_state_observations"] for p in profiles),
                  tools={name: dict(sha256=value) for name, value in tool_hashes.items()},
                  source_sha256={p.name: digest(p) for p in (Path(__file__), fixture, fixture_root / "KvmTransferFixture.h",
                                                           fixture_root / "NativeXStateFixture.h", fixture_root / "verify_frame_returns.py")},
                  scope="KVM original-ENCLU input boundary; zero enclave instructions executed; no successful EPC/SSA/AEX qualification")
    (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
