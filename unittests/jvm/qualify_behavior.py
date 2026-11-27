"""Compare the owned original and reconstructed classes with the same JVM."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from qualify import SOURCES, invoke


def compare(args, row):
    name, harness = {
        "effects": ("Effects", "EffectHarness"),
        "lambda-generics": ("Features", "FeatureHarness"),
    }.get(row["id"], ("Sample", "Harness"))
    data = base64.b64decode(row["bytes"], validate=True)
    assert hashlib.sha256(data).hexdigest() == row["sha256"]
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        source, recovered = root / "original", root / "recovered"
        source.mkdir()
        recovered.mkdir()
        artifact = source / f"{name}.class"
        artifact.write_bytes(data)
        result = subprocess.run(
            [args.cli, "java", artifact, "--json"], capture_output=True, check=False
        )
        report = json.loads(result.stdout)
        assert report["input_sha256"] == row["sha256"]
        bodies = []
        for method in report["functions"]:
            if name == "Features" and method["name"] not in {
                "add",
                "describe",
                "locked",
                "lambda$add$0",
            }:
                continue
            if method.get("kind") == "native_declaration":
                continue
            assert method["status"] == "reconstructed_source", (
                row["id"],
                method["name"],
                method["diagnostics"],
            )
            assert method["source_map"]
            bodies.append(method["source"])
        file = recovered / f"{name}.java"
        field = "public static int seen;\n" if name == "Effects" else ""
        file.write_text(
            f"public final class {name} {{\n{field}" + "\n".join(bodies) + "}\n"
        )
        flags = row["flags"] if row["id"] != "ecj" else ["--release", "17"]
        compile = [args.jdk / "bin/javac", *flags]
        invoke([*compile, "-d", recovered, file])
        outputs = []
        for directory in [source, recovered]:
            invoke(
                [
                    *compile,
                    "-cp",
                    directory,
                    "-d",
                    directory,
                    SOURCES / f"{harness}.java",
                ]
            )
            outputs.append(invoke([args.jdk / "bin/java", "-cp", directory, harness]))
        assert outputs[0] == outputs[1], row["id"]
        print(f"{row['id']}: {len(outputs[0].splitlines())} observations")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("id")
    p.add_argument("--jdk", required=True, type=Path)
    p.add_argument("--cli", required=True, type=Path)
    args = p.parse_args()
    rows = json.loads((SOURCES / "classes.json").read_text())["fixtures"]
    compare(args, next(r for r in rows if r["id"] == args.id))
