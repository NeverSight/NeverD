#!/usr/bin/env python3
"""Compare exported DIV/IDIV continuations with original hardware instructions."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True)
    args = parser.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[3]
    tools = {"neverc": args.compiler.resolve(),
             "llc": (args.llvm_bin / "llc").resolve(),
             "clang": Path(shutil.which("clang") or "")}
    for name, path in tools.items():
        if not path.is_file():
            parser.error(f"missing {name}: {path}")
    records, profiles = [], []
    environment = dict(os.environ, TMPDIR=str(args.keep.resolve()))

    def run(command, stem, expected=0):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=120, env=environment)
        (args.keep / (stem + ".stdout")).write_text(result.stdout)
        (args.keep / (stem + ".stderr")).write_text(result.stderr)
        records.append({"command": command, "exit_code": result.returncode,
                        "expected_exit_code": expected,
                        "stdout": stem + ".stdout", "stderr": stem + ".stderr"})
        (args.keep / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{stem}: exit {result.returncode}\n{result.stderr}")
        return result.stdout

    oracle = root / "unittests/devirtualization/x86_64/fixtures/x86_division_oracle.h"
    # A signal from generated code must not resume an expired oracle frame.
    negative = args.keep / "expired-oracle.c"
    negative.write_text(f'#include "{oracle}"\n' + """
int main(void) {
  uint64_t a=0,d=0;
  division_install();
  if(!division_native(8,0,1,0,0,&a,&d))return 1;
  if(division_native(8,0,13,0,3,&a,&d)||a!=4||d!=1)return 2;
  raise(SIGFPE);
  return 3;
}
""")
    negative_exe = args.keep / "expired-oracle"
    run([tools["clang"], "-std=gnu11", "-O2", negative, "-o", negative_exe],
        "expired-oracle-compile")
    run([negative_exe], "expired-oracle-run", expected=125)

    for kind, comparisons in (("division", 12000), ("division-event", 1728)):
        for route in ("high", "llvm"):
            source = args.sources / f"{kind}-{route}.c"
            if not source.is_file():
                parser.error("capture both DivisionWidthsAndAliasesMatchNativeFaults "
                             "and EventDeliveryRoutesEveryRegisterDivisionWidth first")
            if kind == "division-event":
                manifest = json.loads(source.with_suffix(".json").read_text())
                original = source.with_suffix(".source.c")
                if (manifest["source_sha256"] != digest(original) or
                        not source.read_bytes().startswith(original.read_bytes()) or
                        manifest["native_requests"] != 0):
                    raise RuntimeError("event source does not match its admitted manifest")
            for level in ("O0", "O2"):
                stem = f"{kind}-{route}-{level}"
                ir = args.keep / (stem + ".ll")
                flags = ["--target=x86_64-unknown-linux-gnu", "-std=c23", "-" + level,
                         "-fno-lto", "-fno-builtin-mimalloc", "-fno-pic", "-fno-pie"]
                run([tools["neverc"], *flags, "-S", "-emit-llvm", source, "-o", ir],
                    stem + "-ir")
                for backend in ("neverc", "llvm-legacy", "llvm-new"):
                    name = stem + "-" + backend
                    obj, binary = (args.keep / (name + suffix) for suffix in (".o", ".exe"))
                    if backend == "neverc":
                        run([tools["neverc"], *flags, "-c", source, "-o", obj],
                            name + "-compile")
                    else:
                        run([tools["llc"], "-mtriple=x86_64-unknown-linux-gnu",
                             "-filetype=obj", "-verify-machineinstrs", "-" + level,
                             *(["-enable-new-pm"] if backend == "llvm-new" else []),
                             ir, "-o", obj], name + "-compile")
                    run([tools["clang"], "-no-pie", obj, "-o", binary], name + "-link")
                    output = run([binary], name + "-run")
                    noun = "event" if kind == "division-event" else "state"
                    if output.strip() != f"{comparisons} native DIV/IDIV {noun} comparisons":
                        raise RuntimeError(f"{name}: incomplete hardware comparison output")
                    profiles.append({"kind": kind, "route": route, "optimization": level,
                                     "backend": backend, "source_sha256": digest(source),
                                     "ir_sha256": digest(ir), "object_sha256": digest(obj),
                                     "original_cpu_comparisons": comparisons})
                    print(f"PASS: {name}: {comparisons} comparisons", flush=True)

    report = {"schema_version": 1,
              "tools": {name: {"path": str(path), "sha256": digest(path)}
                        for name, path in tools.items()},
              "oracle_sha256": digest(oracle), "profiles": profiles,
              "expired_oracle_signal_exit": 125,
              "original_cpu_comparisons": sum(p["original_cpu_comparisons"] for p in profiles),
              "limitations": ["original CPU oracle executes register DIV/IDIV in long mode",
                              "event frames follow the existing qualified IA-32e delivery owner",
                              "no memory divisors, nested delivery faults or legacy i386 dispatcher"]}
    (args.keep / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(profiles)} profiles, {report['original_cpu_comparisons']} CPU comparisons")


if __name__ == "__main__":
    main()
