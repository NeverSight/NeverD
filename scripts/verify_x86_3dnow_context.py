#!/usr/bin/env python3
"""Execute exported NeverD owned 3DNow probes against their original bytes.

Run NeverDX86ThreeDNowContextTests with NEVERD_ISA_REGRESSION_ARTIFACT_DIR,
then pass that directory here. Uses GNU binutils, Clang and QEMU system TCG.
The guest compares all retained x87 fields and MMX payloads across TOP/tags/RC,
plus #MF/#PF/#GP/#SS and absent-feature #UD. This qualifies the specified QEMU
processor model; it does not certify physical CPUs or an entire machine ABI.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess

FIXTURES = Path(__file__).resolve().parents[1] / "unittests/fixtures/x86/3dnow-context"


def run(command, directory):
    result = subprocess.run(list(map(str, command)), cwd=directory, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")


def verify(args):
    directory = args.keep
    directory.mkdir(parents=True, exist_ok=True)
    for source in FIXTURES.iterdir():
        if source.suffix in (".S", ".ld", ".c"):
            shutil.copyfile(source, directory / source.name)
    probes = sorted(args.artifacts.rglob("probe.json"), key=lambda p: int(p.parent.name))
    if len(probes) != 96:
        raise ValueError(f"expected 24 operations x 4 source forms; found {len(probes)}")
    headers, table, oracle, objects, inventory = [], [], [".text\n"], [], []
    for number, path in enumerate(probes):
        spec = json.loads(path.read_text())
        if spec["format"] != "ELF":
            raise ValueError("the guest requires x64 ELF exports")
        name = f"original_{number}"
        headers.append(f"extern void {name}(const U64 *);\n")
        oracle.append(f".global {name}\n{name}:\n.byte " + ",".join(map(str, spec["bytes"])) + "\n")
        compiled = sorted(path.parent.glob("*.o"))
        if len(compiled) != 10:
            raise ValueError(f"expected ten compilation profiles in {path.parent}; found {len(compiled)}")
        for obj in compiled:
            index = len(objects)
            output = f"compiled-{index}.o"
            run(["objcopy", "--redefine-sym", f"isa_probe=compiled_{index}", obj, output], directory)
            headers.append(f"extern void compiled_{index}(const U64 *);\n")
            table.append(f"{{compiled_{index}, {name}, {spec['form']}}},\n")
            objects.append(output)
            inventory.append({"source": str(obj), "sha256": hashlib.sha256(obj.read_bytes()).hexdigest(),
                              "opcode": spec["opcode"], "form": spec["form"], "index": index})
    (directory / "cases.inc").write_text("".join(headers) + "static const struct Case cases[] = {\n" + "".join(table) + "};\n")
    (directory / "oracle.S").write_text("".join(oracle) + '.section .note.GNU-stack,"",@progbits\n')
    run([args.clang, "--target=x86_64-unknown-none", "-ffreestanding", "-fno-stack-protector",
         "-mno-red-zone", "-mgeneral-regs-only", f"-DALLOW_QEMU_MISSING_MF={int(args.allow_missing_mf)}", f"-DALLOW_QEMU_STACK_GP={int(args.allow_stack_gp)}", "-O2", "-c", "guest.c", "-o", "guest.o"], directory)
    for name in ("entry", "oracle"):
        run(["as", "--64", name + ".S", "-o", name + ".o"], directory)
    run(["ld", "-m", "elf_x86_64", "-T", "payload.ld", "entry.o", "oracle.o", "guest.o", *objects, "-o", "payload.elf"], directory)
    run(["objcopy", "-O", "binary", "payload.elf", "payload.bin"], directory)
    if (directory / "payload.bin").stat().st_size >= 0x3d00000:
        raise ValueError("guest payload would overlap the reserved stack")
    run(["as", "--32", "boot.S", "-o", "boot.o"], directory)
    run(["ld", "-m", "elf_i386", "-T", "boot.ld", "boot.o", "-o", "boot.elf"], directory)
    results = []
    for label, cpu, normal, faults in (("enabled", "max", len(objects) * 384, len(objects) * 5 // 2),
                                       ("disabled", "max,-3dnow,-3dnowext", 0, len(objects) * 7 // 2)):
        command = [args.qemu, "-machine", "pc,accel=tcg", "-cpu", cpu, "-m", "64M", "-nodefaults",
                   "-display", "none", "-no-reboot", "-monitor", "none", "-serial", "none",
                   "-debugcon", "stdio", "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-kernel", "boot.elf"]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=args.timeout)
        (directory / (label + ".log")).write_text(result.stdout + result.stderr)
        match = re.fullmatch(r"PASS:([0-9a-f]{8}):([0-9a-f]{8}):([0-9a-f]{8}):([0-9a-f]{8})\n", result.stdout)
        counts = tuple(int(value, 16) for value in match.groups()) if match else ()
        if result.returncode != 1 or not match or counts[0] != normal or sum(counts[1:]) != faults or (counts[2] and not args.allow_missing_mf) or (counts[3] and not args.allow_stack_gp):
            raise RuntimeError(f"{label}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
        results.append({"cpu": cpu, "state_comparisons": normal, "fault_comparisons": counts[1],
                        "unqualified_pending_mf_differentials": counts[2], "unqualified_stack_ss_differentials": counts[3], "fwait_mf_control": "passed"})
        print(f"{label}: {normal} state, {counts[1]} fault comparisons passed; {counts[2]} pending-MF and {counts[3]} stack-SS cases unqualified by this CPU model", flush=True)
    (directory / "qualification.json").write_text(json.dumps({"results": results, "objects": inventory}, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=lambda p: Path(p).resolve())
    parser.add_argument("--keep", required=True, type=lambda p: Path(p).resolve())
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--allow-missing-mf", action="store_true",
                        help="Record the QEMU 10.2.2 missing 3DNow #MF model behavior as unqualified, never as passed fault checks")
    parser.add_argument("--allow-stack-gp", action="store_true",
                        help="Record QEMU reporting #GP for noncanonical RBP references as unqualified #SS checks")
    parser.add_argument("--timeout", default=120, type=int)
    verify(parser.parse_args())


if __name__ == "__main__":
    main()
