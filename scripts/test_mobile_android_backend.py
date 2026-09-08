#!/usr/bin/env python3
"""Exercise real smali/DEX/multidex recovery, then compile and run recovered Java.

Requires JADX 1.5.6+ distribution and a JDK. Nothing is downloaded or installed.
Example: python3 scripts/test_mobile_android_backend.py --neverd build/bin/neverd --jadx /opt/jadx/bin/jadx
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]


ASSEMBLER = """
import com.android.tools.smali.smali.Smali;
import com.android.tools.smali.smali.SmaliOptions;
import java.util.Arrays;
public class AssembleFixture {
    public static void main(String[] args) throws Exception {
        SmaliOptions options = new SmaliOptions();
        options.apiLevel = 27;
        options.outputDexFile = args[0];
        if (!Smali.assemble(options, Arrays.asList(args).subList(1, args.length))) {
            throw new IllegalStateException("Fixture assembly failed");
        }
    }
}
"""

HARNESS = """
import fixture.Calculator;
import fixture.Peer;
public class VerifyRecovered {
    public static void main(String[] args) {
        if (Calculator.compute(9) != 21 || Calculator.compute(-5) != -7
            || Calculator.sumAbs(new int[]{-3, 2, -9, 0}) != 14
            || Calculator.sumAbs(new int[]{}) != 0
            || Calculator.safeDivide(21, 3) != 7
            || Calculator.safeDivide(21, 0) != -1
            || Calculator.Nested.bump(4) != 7
            || !Peer.greeting().equals("neverd")) {
            throw new AssertionError("Recovered Java changed fixture behavior");
        }
        System.out.println("recovered Java behavior verified");
    }
}
"""

SINGLE_HARNESS = """
import fixture.Peer;
public class VerifyRecovered {
    public static void main(String[] args) {
        if (Peer.twice(9) != 18 || !Peer.greeting().equals("neverd")) {
            throw new AssertionError("Recovered Java changed fixture behavior");
        }
        System.out.println("single smali behavior verified");
    }
}
"""


def run(argv: list[str]) -> None:
    completed = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=120, check=False)
    if completed.returncode:
        raise RuntimeError(f"{Path(argv[0]).name} exited {completed.returncode}:\n{completed.stdout}")


def java_tool(name: str) -> str:
    suffix = ".exe" if os.name == "nt" else ""
    home = os.environ.get("JAVA_HOME")
    path = Path(home) / "bin" / (name + suffix) if home else None
    result = str(path) if path and path.is_file() else shutil.which(name)
    if not result:
        raise RuntimeError(f"{name} is required; set JAVA_HOME to a JDK")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jadx", required=True, type=Path)
    parser.add_argument("--jadx-jar", type=Path, help="distribution all.jar for fixture assembly")
    parser.add_argument("--neverd", type=Path, required=True, help="native C++ NeverD CLI under test")
    arguments = parser.parse_args()
    jadx = arguments.jadx.resolve()
    neverd = arguments.neverd.resolve()
    if not neverd.is_file():
        raise RuntimeError("--neverd must point to the built CLI")
    jars = [arguments.jadx_jar.resolve()] if arguments.jadx_jar else list((jadx.parent.parent / "lib").glob("jadx-*-all.jar"))
    if len(jars) != 1:
        raise RuntimeError("Pass --jadx-jar or use the bin/jadx launcher from a full distribution")
    jar = jars[0]
    java, javac = java_tool("java"), java_tool("javac")
    fixtures = ROOT / "scripts" / "tests" / "fixtures" / "mobile"
    with tempfile.TemporaryDirectory(prefix="neverd-android-smoke-") as temporary:
        work = Path(temporary)
        helper = work / "AssembleFixture.java"
        helper.write_text(ASSEMBLER)
        run([javac, "-cp", str(jar), str(helper)])

        def assemble(destination: Path, inputs: list[Path]) -> None:
            run([java, "-cp", os.pathsep.join([str(work), str(jar)]), "AssembleFixture",
                 str(destination), *map(str, inputs)])

        def verify(source: Path, name: str, *, single: bool = False) -> dict:
            output = work / name
            run([str(neverd), "mobile", str(source), "-o", str(output), "--jadx", str(jadx)])
            report = json.loads((output / "report.json").read_text())
            harness = output / "VerifyRecovered.java"
            harness.write_text(SINGLE_HARNESS if single else HARNESS)
            compiled = output / "compiled"
            compiled.mkdir()
            run([javac, "-d", str(compiled), str(harness), *map(str, (output / "sources").rglob("*.java"))])
            run([java, "-cp", str(compiled), "VerifyRecovered"])
            print(f"PASS {name}: {report['java_source_count']} Java files, compiled and executed")
            return report

        verify(fixtures / "Peer.smali", "single-smali", single=True)
        verify(fixtures, "smali-directory")
        combined_dex = work / "combined.dex"
        assemble(combined_dex, [fixtures])
        verify(combined_dex, "dex")
        first = work / "classes.dex"
        second = work / "classes2.dex"
        assemble(first, [fixtures / "Calculator.smali", fixtures / "Calculator$Nested.smali"])
        assemble(second, [fixtures / "Peer.smali"])
        apk = work / "multidex.apk"
        with zipfile.ZipFile(apk, "w") as archive:
            archive.write(first, "classes.dex")
            archive.write(second, "classes2.dex")
            archive.writestr("assets/fixture.txt", "resource intentionally not decompiled")
        report = verify(apk, "multidex-apk")
        if report["dex_count"] != 2:
            raise AssertionError("Multidex report omitted bytecode")
        broken = work / "mixed-broken-smali"
        broken.mkdir()
        shutil.copyfile(fixtures / "Peer.smali", broken / "Peer.smali")
        (broken / "Broken.smali").write_text(".class public Lfixture/Broken;\n.super Ljava/lang/Object;\n.method public bad()I\nnot-an-opcode\n.end method\n")
        duplicate = work / "duplicate-smali"
        duplicate.mkdir()
        shutil.copyfile(fixtures / "Peer.smali", duplicate / "Peer.smali")
        shutil.copyfile(fixtures / "Peer.smali", duplicate / "PeerCopy.smali")
        for label, source in (("malformed", broken), ("duplicate", duplicate)):
            output = work / (label + "-output")
            result = subprocess.run([str(neverd), "mobile", str(source), "-o",
                                     str(output), "--jadx", str(jadx), "--json"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, timeout=120)
            if result.returncode != 1 or json.loads(result.stdout).get("status") != "error":
                raise AssertionError(f"{label} input did not produce a structured recovery error")
            if output.exists():
                raise AssertionError(f"{label} recovery published partial output")
            print(f"PASS {label} input rejects partial success through native CLI")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
