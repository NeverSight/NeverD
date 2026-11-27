"""Record or verify an owned disk fixture with its matching CPython interpreter."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

FIXTURES = Path(__file__).resolve().parent / "fixtures"
SOURCE = FIXTURES / "release_case.py"
MANIFEST = FIXTURES / "releases.json"
OBSERVE = """import release_case
print((release_case.base, release_case.combine(3,4), release_case.combine(-2,8), release_case.choose(0,9), release_case.choose(2,9)))
"""
EXPECTED = "(7, 21, -12, 9, 11)"


def observation(version, bytecode, listing):
    """Take instruction names and offsets from the producer's disassembler."""
    functions = []
    for line in listing.splitlines():
        if line.startswith("@@ "):
            functions.append({"name": line[3:], "instructions": []})
            continue
        match = re.search(r"\b(\d+)\s+(?:([A-Z][A-Z_0-9+]*)\b|<(\d+)>)", line)
        if functions and match:
            mnemonic = match[2]
            if mnemonic is None:
                # The 1.3 VM added MAKE_FUNCTION before dis learned its name.
                if version != "1.3" or match[3] != "132":
                    raise ValueError(f"{version} dis printed unknown opcode {match[3]}")
                mnemonic = "MAKE_FUNCTION"
            functions[-1]["instructions"].append([int(match[1]), mnemonic])
    if len(functions) != 3 or any(not f["instructions"] for f in functions):
        raise ValueError(f"{version} did not list all three code objects")
    return {
        "version": version,
        "magic": bytecode[:4].hex(),
        "sha256": hashlib.sha256(bytecode).hexdigest(),
        "data": base64.b64encode(bytecode).decode("ascii"),
        "functions": functions,
    }


def capture(version, run, work):
    """Only the source checked into this directory and its output are executed."""
    parts = tuple(int(n) for n in version.split(".")[:2])
    code = work / "release_case.py"
    shutil.copyfile(SOURCE, code)
    os.utime(code, (946684800, 946684800))
    compile_file = "import release_case\n"
    if parts >= (1, 5):
        compile_file = (
            "import py_compile\n"
            "py_compile.compile('release_case.py', 'release_case.pyc', 'release_case.py')\n"
        )
    run(compile_file)
    bytecode = (work / "release_case.pyc").read_bytes()
    header = 16 if parts >= (3, 7) else 12 if parts >= (3, 3) else 8
    if run(OBSERVE).strip() != EXPECTED:
        raise ValueError(f"{version} produced unexpected behavior")
    open_file = "'rb'" if parts >= (1, 0) else "'r'"
    display = "dis.disassemble(c, -1)"
    if parts >= (3, 4):
        display = (
            "for i in dis.get_instructions(c): print('%d %s' % (i.offset, i.opname))"
        )
    # Some early code objects store names only in the enclosing binding.
    listing = run(
        "import dis, marshal, sys\n"
        f"f = open('release_case.pyc', {open_file})\nf.read({header})\n"
        "c = marshal.load(f)\n"
        "def dump(c, name):\n"
        " print('@@ ' + name)\n"
        f" {display}\n"
        "dump(c, '<module>')\n"
        "names = ['combine', 'choose']\n"
        "n = 0\n"
        "for item in c.co_consts:\n"
        " if type(item) == type(c):\n"
        "  dump(item, names[n])\n"
        "  n = n+1\n"
    )
    record = observation(version, bytecode, listing)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("interpreter", type=Path)
    parser.add_argument("version", help="Exact release of the interpreter")
    parser.add_argument("--record", action="store_true")
    parser.add_argument(
        "--cli", type=Path, help="Also execute the recovered owned program"
    )
    args = parser.parse_args()
    interpreter = args.interpreter.resolve()
    version = tuple(map(int, args.version.split(".")[:2]))
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)

        def run(program):
            flags = ["-S"] if version >= (1, 5) else []
            return subprocess.check_output(
                [interpreter, *flags, "-c", program], cwd=work, text=True
            )

        actual = run("import sys\nprint(sys.version)").split()[0]
        if actual != args.version:
            raise ValueError(f"Expected CPython {args.version}, got {actual}")
        record = capture(args.version, run, work)
        if args.cli:
            profile = args.version if version < (1, 0) else ".".join(map(str, version))
            report = json.loads(
                subprocess.check_output(
                    [
                        args.cli.resolve(),
                        "python",
                        work / "release_case.pyc",
                        "--json",
                        f"--profile=cpython-{profile}",
                    ],
                    cwd=work,
                )
            )
            if report["status"] != "reconstructed_source":
                raise ValueError(f"{args.version}: {report['status']}")
            (work / "recovered.py").write_text(
                report["functions"][report["root"]]["source"]
            )
            if run(OBSERVE.replace("release_case", "recovered")).strip() != EXPECTED:
                raise ValueError(f"{args.version} recovered behavior differs")
            if version >= (2, 7):
                shutil.copyfile(
                    FIXTURES / "release_calls.py", work / "release_calls.py"
                )
                run(
                    "import py_compile\npy_compile.compile('release_calls.py', 'calls.pyc', 'release_calls.py')"
                )
                result = json.loads(
                    subprocess.check_output(
                        [args.cli.resolve(), "python", work / "calls.pyc", "--json"],
                        cwd=work,
                    )
                )
                if result["status"] != "reconstructed_source":
                    raise ValueError(f"{args.version} call reconstruction differs")
                (work / "calls_result.py").write_text(
                    result["functions"][result["root"]]["source"]
                )
                exercise = (
                    "import release_calls\ndef add(a,b): return a+b\n"
                    "print((release_calls.visit(add,6,9), release_calls.collect([7,0,7])))"
                )
                if (
                    run(exercise).strip() != "(15, 2)"
                    or run(exercise.replace("release_calls", "calls_result")).strip()
                    != "(15, 2)"
                ):
                    raise ValueError(f"{args.version} call behavior differs")
    rows = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else []
    if args.record:
        rows = [row for row in rows if row["version"] != args.version] + [record]
        rows.sort(key=lambda row: tuple(map(int, row["version"].split("."))))
        MANIFEST.write_text(
            "[\n" + ",\n".join(json.dumps(row) for row in rows) + "\n]\n"
        )
    elif record not in rows:
        raise ValueError(f"{args.version} differs from the committed fixture")


if __name__ == "__main__":
    main()
