#!/usr/bin/env python3
"""Execute GNU-assembled leaves and the SDK's recompiled ELF objects in QEMU."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "pluginsdk/python"))
from neverd_plugin.ffi import HostAPI


def lift_object(api, work):
    session = api.call("neverd_session_create")
    if not session:
        raise RuntimeError("cannot open NeverD session")
    try:
        if api.call("neverd_lift_to_obj", session, os.fsencode(work / "input.elf"), 0, 1):
            diagnostic = api.owned_string("neverd_last_error", session)
            if api.borrowed_bytes("neverd_roundtrip_obj", session) or api.call(
                "neverd_roundtrip_func_count", session
            ):
                raise ValueError("failed lift published a partial object")
            return None, None, diagnostic
        if api.call("neverd_roundtrip_func_count", session) != 1:
            raise ValueError("expected one defined function")
        name = api.owned_string("neverd_roundtrip_func_name", session, 0)
        size = api.call("neverd_roundtrip_func_size", session, 0)
        data = api.borrowed_bytes("neverd_roundtrip_obj", session)
        if not name or not size or not data:
            raise ValueError("missing native entry or ELF object")
        (work / "rebuilt.o").write_bytes(data)
        return name, hashlib.sha256(data).hexdigest(), None
    finally:
        api.call("neverd_session_destroy", session)


def execute_profile(api, work, abi, little, mode):
    wide = abi != "o32"
    suffix = "el" if little else ""
    prefix = f"mips{'64' if wide else ''}{suffix}-linux-gnu{'abi64' if wide else ''}-"
    cpu = "MIPS64R2-generic" if wide else "74Kf"
    isa = "mips64r2" if wide else "mips32r2"
    float_flags = ["-msoft-float"]
    if mode == "micromips":
        cpu = "M14Kc"
    elif mode in ("mips1", "mips2", "mips3", "mips4", "mips5"):
        isa = mode
        float_flags = ["-mhard-float"]
    elif mode == "r6":
        cpu = "I6400" if wide else "mips32r6-generic"
        isa = "mips64r6" if wide else "mips32r6"
        float_flags = ["-mhard-float", "-mfp64", "-mnan=2008"]
    emulator = "qemu-mips" + {"o32": "", "n32": "n32", "n64": "64"}[abi] + suffix
    emulation = ("elf64" if abi == "n64" else "elf32") + ("ltsmip" if little else "btsmip")
    if abi == "n32":
        emulation += "n32"
    assembler = [prefix + "as", {"o32": "-32", "n32": "-n32", "n64": "-64"}[abi],
                 "-march=" + isa, *float_flags]
    linker = [prefix + "ld", "-m", emulation, "-Ttext", "0x410000", "--build-id=none"]

    def run(command):
        subprocess.run(command, cwd=work, check=True, timeout=30)

    directive = ".set " + mode if mode in ("mips16", "micromips") else ""
    body = "jr $31\n addiu $2,$4,7"
    argument = "addiu $4,$0,30"
    check = ""
    if mode == "r6":
        body = "addiu $2,$4,7\n jrc $31"
    if mode == "mips16":
        body = "li $2,37\n jrc $31"
    if wide and mode in ("mips3", "mips4", "mips5"):
        # Check both words of the n32/n64 result on a compatible 64-bit CPU.
        body = "dsrl32 $3,$4,0\n jr $31\n daddu $2,$3,$4"
        argument = "ori $4,$0,7\n dsll32 $4,$4,0\n ori $4,$4,30"
        check = "dsrl32 $3,$2,0\n xori $3,$3,7\n bne $3,$0,failed\n nop"
    (work / "leaf.s").write_text(f""".set noreorder
{directive}
.text
.globl leaf
.ent leaf
leaf:
 {body}
.end leaf
""")
    run([*assembler, "leaf.s", "-o", "original.o"])
    run([*linker, "-e", "leaf", "original.o", "-o", "input.elf"])
    entry, digest, diagnostic = lift_object(api, work)
    variants = [("original", "leaf")]
    if mode in ("mips1", "mips2", "mips3"):
        # Function entry cannot establish the incoming I–III pipeline state.
        context_error = "requires an authenticated execution context"
        if entry or context_error not in (diagnostic or ""):
            raise RuntimeError(
                f"{abi}/{mode}: expected historical execution context diagnostic, got {diagnostic!r}"
            )
    elif not entry:
        raise RuntimeError(f"{abi}/{mode}: {diagnostic}")
    else:
        variants.append(("rebuilt", entry))
    exit_syscall = {"o32": 4001, "n32": 6058, "n64": 5058}[abi]
    for variant, symbol in variants:
        (work / "start.s").write_text(f""".set noreorder
.option pic0
.text
.globl _start
.ent _start
_start:
 {argument}
 lui $25,%hi({symbol})
 addiu $25,$25,%lo({symbol})
 jalr $25
 nop
 {check}
 move $4,$2
 li $2,{exit_syscall}
 syscall
failed:
 li $4,99
 li $2,{exit_syscall}
 syscall
.end _start
""")
        run([*assembler, "start.s", "-o", "start.o"])
        run([*linker, "-e", "_start", "start.o", variant + ".o", "-o", "program.elf"])
        result = subprocess.run([emulator, "-cpu", cpu, "program.elf"], cwd=work, timeout=10)
        if result.returncode != 37:
            raise RuntimeError(f"{abi} {suffix or 'be'} {mode} {variant}: exit {result.returncode}")
    row = dict(abi=abi, endian="little" if little else "big", mode=mode, cpu=cpu, original=37)
    if entry:
        row.update(entry=entry, object_sha256=digest, rebuilt=37)
    else:
        row["diagnostic"] = diagnostic
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="JSON result file")
    args = parser.parse_args()
    api = HostAPI(ctypes.CDLL(str(args.sdk.resolve())))
    rows = []
    for abi in ("o32", "n32", "n64"):
        if abi == "o32":
            modes = ("classic", "micromips", "mips16", "mips1", "mips2", "r6")
        else:
            modes = ("classic", "mips3", "mips4", "mips5", "r6")
        for little in (False, True):
            for mode in modes:
                with tempfile.TemporaryDirectory(prefix="neverd-mips-native-") as directory:
                    rows.append(execute_profile(api, Path(directory), abi, little, mode))
    args.output.write_text(json.dumps(rows, indent=2) + "\n")
    rebuilt = sum("rebuilt" in row for row in rows)
    print(f"{len(rows)} GNU originals, {rebuilt} SDK objects and "
          f"{len(rows) - rebuilt} historical context diagnostics passed")


if __name__ == "__main__":
    main()
