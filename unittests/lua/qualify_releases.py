"""Recompile and run a single owned release fixture with its recorded tools."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures"


def run(command, directory):
    return subprocess.run(
        command, cwd=directory, capture_output=True, check=True
    ).stdout


def compile_fixture(row, compiler, directory):
    name = row["input_name"]
    dialect = row["dialect"]
    target = directory / "output.luac"
    if dialect == "puc":
        listing = run([compiler, "-l", "-o", target.name, name], directory)
    elif dialect == "luajit":
        run([compiler, "-bg", name, target.name], directory)
        listing = run([compiler, "-bl", name], directory)
    else:
        prefix = "--compile=" if row["combined_cli"] else "--"
        args = ["-O0", *row.get("flags", [])]
        target.write_bytes(run([compiler, prefix + "binary", *args, name], directory))
        listing = run([compiler, prefix + "text", *args, name], directory)
    return target, listing.decode()


def observations(row, listing):
    """Read only the matching compiler's listing, including physical PCs when printed."""
    groups = []
    dialect = row["dialect"]
    for line in listing.splitlines():
        if dialect == "luau":
            if re.match(r"^Function \d+ ", line):
                groups.append([])
            insn = re.match(r"^(?:L\d+: )?([A-Z][A-Z0-9_]*)\b", line)
            if insn and insn[1] != "REMARK":
                groups[-1].append([None, insn[1]])
        elif dialect == "luajit":
            if line.startswith("-- BYTECODE --"):
                groups.append([])
            insn = re.match(r"^\s*(\d+)\s+(?:=>\s+)?([A-Z][A-Z0-9_]*)\b", line)
            if insn:
                groups[-1].append([int(insn[1]) - 1, insn[2]])
        else:
            if re.match(r"^(main|function)\b", line):
                groups.append([])
            insn = re.match(
                r"^\s*(\d+)\s+(?:\[\d+\]\s+|[0-9A-F]{2,}\s+)?([A-Z][A-Z_0-9]+)\b",
                line,
            )
            if insn and insn[2] not in ("ARGS", "STACK"):
                groups[-1].append([int(insn[1]) - row["pc_origin"], insn[2]])
    if not groups or not all(groups):
        raise ValueError(f"Incomplete producer listing for {row['id']}")
    return groups


def compare_code(row, report, expected):
    assert report["status"] == "reconstructed_source", report["diagnostics"]
    actual = []
    for function in report["functions"]:
        assert function["status"] == "reconstructed_source", function["diagnostics"]
        if row["dialect"] == "luau":
            # The compiler's text dump suppresses its implicit vararg prologue.
            actual.append(
                [
                    [None, i["opcode"]]
                    for i in function["instructions"]
                    if i["opcode"] != "PREPVARARGS"
                ]
            )
        else:
            actual.append([[i["pc"], i["opcode"]] for i in function["instructions"]])
    assert actual == expected, (actual, expected)


def compare_bytes(row, fresh):
    recorded = bytearray(base64.b64decode(row["bytes"], validate=True))
    assert hashlib.sha256(recorded).hexdigest() == row["sha256"]
    actual = bytearray(fresh)
    for start, width in row.get("relocated_fields", []):
        # Lua 2 dumps store a producer-process pointer placeholder at these sites.
        actual[start : start + width] = recorded[start : start + width]
    assert actual == recorded


def behavior(row, directory, runtime, source, recovered):
    if row["dialect"] == "luau":
        for program in (source, recovered):
            (directory / "driver.lua").write_text(
                "local function program()\n" + program + "\nend\nprint(program())\n"
            )
            assert run([runtime, "driver.lua"], directory).strip() == b"16"
        return
    for value in (8, -17):
        for program in (source, recovered):
            (directory / "owned.lua").write_text(program)
            (directory / "driver.lua").write_text(
                f'argument = {value}\ndofile("owned.lua")\nprint(answer)\n'
            )
            assert float(run([runtime, "driver.lua"], directory).strip()) == value + 7


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("id")
    for name in ("cli", "compiler", "runtime"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    rows = json.loads((FIXTURES / "releases.json").read_text())["fixtures"]
    row = next(item for item in rows if item["id"] == args.id)
    source = (FIXTURES / "releases" / row["source"]).read_text()
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)
        (directory / row["input_name"]).write_text(source)
        target, listing = compile_fixture(row, args.compiler.resolve(), directory)
        compare_bytes(row, target.read_bytes())
        expected = observations(row, listing)
        assert row["instructions"] == expected
        report = json.loads(
            run(
                [
                    args.cli.resolve(),
                    "lua",
                    target,
                    "--json",
                    "--profile",
                    row["profile"],
                ],
                directory,
            )
        )
        compare_code(row, report, expected)
        behavior(
            row,
            directory,
            args.runtime.resolve(),
            source,
            report["functions"][report["root"]]["source"],
        )
    print(f"{row['id']} passed byte, instruction and behavior comparisons")


if __name__ == "__main__":
    main()
