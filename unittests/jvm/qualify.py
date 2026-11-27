"""Regenerate the owned class matrix from local JDKs and javap listings."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
SOURCES = HERE / "fixtures"


def invoke(args, cwd=None, env=None):
    result = subprocess.run(
        args, cwd=cwd, env=env, text=True, capture_output=True, check=False
    )
    if result.returncode:
        raise RuntimeError(f"{args}\n{result.stdout}\n{result.stderr}")
    return result.stdout or result.stderr


def disassembly(listing):
    owner = re.search(r"this_class:\s*#\d+\s*//\s*(\S+)", listing)
    owner = owner[1].replace("/", ".") if owner else ""
    methods = []
    current = None
    descriptor = None
    exceptions = False
    switch = None
    for line in listing.splitlines():
        head = re.match(r"^  ([^ ].*)[;{]$", line)
        if head:
            declaration = head[1]
            match = re.search(r"([^ (\t]+)\(", declaration)
            name = (
                match[1]
                if match
                else "<clinit>"
                if declaration == "static {}"
                else None
            )
            if name == owner:
                name = "<init>"
            descriptor = name
            current = None
        elif descriptor and line.strip().startswith("descriptor:"):
            current = {
                "name": descriptor,
                "descriptor": line.split("descriptor:", 1)[1].strip(),
                "instructions": [],
                "handlers": [],
            }
            methods.append(current)
            descriptor = None
        elif current is not None:
            if switch is not None:
                case = re.match(r"\s*(default|-?\d+):\s*(\d+)\s*$", line)
                if case:
                    if case[1] == "default":
                        switch["default_target"] = int(case[2])
                    else:
                        switch["cases"].append(
                            {"key": int(case[1]), "target": int(case[2])}
                        )
                    continue
                switch = None
            op = re.match(r"^\s+(\d+):\s+([a-z][a-z0-9_]+)\b(.*)", line)
            if op:
                name = op[2]
                operands = op[3].split("//", 1)[0].strip()
                args = [x.strip() for x in operands.split(",")]
                evidence = {}
                if args[0].startswith("#"):
                    evidence["pool_index"] = int(args[0][1:])
                    if name in {"invokeinterface", "multianewarray"}:
                        evidence["operand"] = int(args[1])
                elif name in {"bipush", "sipush"}:
                    evidence["operand"] = int(args[0])
                elif name == "newarray":
                    evidence["operand"] = [
                        "boolean",
                        "char",
                        "float",
                        "double",
                        "byte",
                        "short",
                        "int",
                        "long",
                    ].index(args[0]) + 4
                elif name.startswith("if") or name in {
                    "goto",
                    "goto_w",
                    "jsr",
                    "jsr_w",
                }:
                    evidence["branch_target"] = int(args[0])
                elif name in {"tableswitch", "lookupswitch"}:
                    evidence["cases"] = []
                    switch = evidence
                elif re.fullmatch(r"[ailfd](load|store)(_w)?|ret(_w)?|iinc(_w)?", name):
                    evidence["local"] = int(args[0])
                    if name.startswith("iinc"):
                        evidence["increment"] = int(args[1])
                    if name.endswith("_w"):
                        evidence["wide"] = True
                        name = name[:-2]
                instruction = [int(op[1]), name]
                if evidence:
                    instruction.append(evidence)
                current["instructions"].append(instruction)
            if line.strip() == "Exception table:":
                exceptions = True
            elif exceptions and "target type" not in line:
                handler = re.match(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+(?:Class )?(\S+)", line)
                if handler:
                    current["handlers"].append(
                        [*(int(n) for n in handler.groups()[:3]), handler[4]]
                    )
                elif line.strip():
                    exceptions = False
    return methods


def produce(args):
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def record(identifier, home, flags, sources, cls, producer=None):
            out = root / identifier.replace("/", "-")
            out.mkdir()
            command = producer if producer else [home / "bin/javac"]
            invoke([*command, *flags, "-d", out, *(SOURCES / s for s in sources)])
            artifact = out / (cls + ".class")
            data = artifact.read_bytes()
            listing = invoke(
                [args.current / "bin/javap", "-v", "-p", "-c", "-s", artifact]
            )
            rows.append(
                {
                    "id": identifier,
                    "sources": sources,
                    "flags": flags,
                    "compiler": invoke([*command, "-version"]).strip(),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "major": int.from_bytes(data[6:8], "big"),
                    "minor": int.from_bytes(data[4:6], "big"),
                    "bytes": base64.b64encode(data).decode(),
                    "methods": disassembly(listing),
                }
            )
            if args.corpus:
                dest = args.corpus / identifier
                dest.mkdir(parents=True, exist_ok=True)
                (dest / artifact.name).write_bytes(data)
                (dest / "javap.txt").write_text(listing)

        for release in range(1, args.latest + 1):
            if release < 8:
                home = args.jdk8
                target = f"1.{release}"
                flags = [
                    "-source",
                    "1.3" if release < 4 else target,
                    "-target",
                    target,
                    "-g",
                ]
            elif release in (8, 11, 17, 21):
                home = getattr(args, f"jdk{release}")
                flags = ["-g"]
            else:
                home = args.current
                flags = ["--release", str(release), "-g"]
            record(f"java-{release}", home, flags, ["Sample.java"], "Sample")
        record("stripped", args.jdk17, ["-g:none"], ["Sample.java"], "Sample")
        record(
            "lambda-generics",
            args.jdk17,
            ["-g", "-parameters"],
            ["Features.java"],
            "Features",
        )
        record("record", args.jdk21, ["-g"], ["Point.java"], "Point")
        record(
            "module",
            args.jdk11,
            ["-g:none"],
            ["module-info.java", "Marker.java"],
            "module-info",
        )
        record(
            "ecj",
            args.current,
            ["-17", "-g"],
            ["Sample.java"],
            "Sample",
            [args.current / "bin/java", "-jar", args.ecj],
        )
        record("effects", args.jdk17, ["-g"], ["Effects.java"], "Effects")
        for cls in ("Root", "Middle", "Leaf"):
            record(
                "hierarchy-" + cls.lower(),
                args.jdk17,
                ["-g:none"],
                ["Hierarchy.java"],
                cls,
            )
        for compiler in args.kotlin:
            env = dict(os.environ, JAVA_HOME=str(args.jdk17.resolve()))
            version = invoke([compiler, "-version"], env=env)
            identifier = "kotlin-" + re.search(r"kotlinc-jvm ([0-9.]+)", version)[1]
            out = root / identifier
            out.mkdir()
            flags = ["-jvm-target", "17"]
            invoke([compiler, *flags, SOURCES / "Greeting.kt", "-d", out], env=env)
            artifact = out / "example/Greeting.class"
            data = artifact.read_bytes()
            listing = invoke(
                [args.current / "bin/javap", "-v", "-p", "-s", "-c", artifact]
            )
            if args.corpus:
                dest = args.corpus / identifier
                dest.mkdir(parents=True, exist_ok=True)
                (dest / artifact.name).write_bytes(data)
                (dest / "javap.txt").write_text(listing)
            rows.append(
                {
                    "id": identifier,
                    "sources": ["Greeting.kt"],
                    "flags": flags,
                    "compiler": version.strip(),
                    "major": int.from_bytes(data[6:8], "big"),
                    "minor": int.from_bytes(data[4:6], "big"),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "bytes": base64.b64encode(data).decode(),
                    "methods": disassembly(listing),
                }
            )
    args.manifest.write_text(
        json.dumps({"fixtures": rows}, separators=(",", ":")) + "\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for n in ["jdk8", "jdk11", "jdk17", "jdk21", "current", "ecj"]:
        parser.add_argument("--" + n, type=Path, required=True)
    parser.add_argument("--kotlin", action="append", type=Path, default=[])
    parser.add_argument("--latest", type=int, default=27)
    parser.add_argument("--manifest", type=Path, default=SOURCES / "classes.json")
    parser.add_argument("--corpus", type=Path)
    produce(parser.parse_args())
