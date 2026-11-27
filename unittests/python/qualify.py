"""Rebuild owned fixtures and validate source under their recorded interpreter."""

import argparse
import base64
import dis
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import py_compile
import subprocess
import sys
import tempfile
import types

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
MANIFEST = FIXTURES / "artifacts.json"


def children(code):
    yield code
    for value in code.co_consts:
        if isinstance(value, types.CodeType):
            yield from children(value)


def write_manifest(path, value):
    """Keep each independent disassembly observation on one reviewable line."""

    def encode(item, depth=0):
        if isinstance(item, dict):
            opening, closing = "{", "}"
            rows = [
                json.dumps(key) + ": " + encode(val, depth + 1)
                for key, val in item.items()
            ]
        elif isinstance(item, list) and any(
            isinstance(val, (dict, list)) for val in item
        ):
            opening, closing = "[", "]"
            rows = [encode(val, depth + 1) for val in item]
        else:
            return json.dumps(item, ensure_ascii=False)
        indent = "  " * (depth + 1)
        return (
            opening
            + "\n"
            + indent
            + (",\n" + indent).join(rows)
            + "\n"
            + "  " * depth
            + closing
        )

    path.write_text(encode(value) + "\n", encoding="utf-8")


def record(name, data, **metadata):
    return dict(
        name=name,
        sha256=hashlib.sha256(data).hexdigest(),
        data=base64.b64encode(data).decode("ascii"),
        **metadata,
    )


def generate(folder, clang):
    from PyInstaller import __version__ as packager
    from PyInstaller.archive.writers import CArchiveWriter, ZlibArchiveWriter

    if sys.version_info[:3] != (3, 10, 16) or packager != "6.16.0":
        raise ValueError(
            "Fixture generation requires CPython 3.10.16 and PyInstaller 6.16.0"
        )
    rows = []
    cases = [
        ("program", 0, "CHECKED_HASH"),
        ("held_out", 0, "TIMESTAMP"),
        ("optimization", 1, "UNCHECKED_HASH"),
        ("optimization", 2, "CHECKED_HASH"),
    ]
    for stem, opt, mode in cases:
        source = folder / (stem + ".py")
        source.write_bytes((FIXTURES / source.name).read_bytes())
        os.utime(source, (1700000000, 1700000000))
        dest = folder / (stem + str(opt) + ".pyc")
        py_compile.compile(
            str(source),
            str(dest),
            dfile=source.name,
            doraise=True,
            optimize=opt,
            invalidation_mode=getattr(py_compile.PycInvalidationMode, mode),
        )
        code = compile(
            source.read_bytes(), source.name, "exec", dont_inherit=True, optimize=opt
        )
        oracle = []
        for child in children(code):
            lines = {
                p: line
                for start, end, line in child.co_lines()
                for p in range(start, end, 2)
            }
            oracle.append(
                dict(
                    name=child.co_name,
                    instructions=[
                        [
                            i.offset,
                            i.opname,
                            lines.get(i.offset),
                            i.argval if i.opcode in dis.hasjabs + dis.hasjrel else None,
                        ]
                        for i in dis.get_instructions(child)
                    ],
                )
            )
        rows.append(
            record(
                dest.name,
                dest.read_bytes(),
                source=source.name,
                optimize=opt,
                header=mode.lower(),
                disassembly=oracle,
            )
        )
    native = folder / "owned.so"
    subprocess.run(
        [
            clang,
            "-target",
            "x86_64-unknown-linux-gnu",
            "-c",
            FIXTURES / "native.s",
            "-o",
            native,
        ],
        check=True,
    )
    rows.append(
        record(native.name, native.read_bytes(), producer="owned x86_64 ELF assembly")
    )
    pyz = folder / "members.pyz"
    unit = compile(
        (FIXTURES / "held_out.py").read_bytes(),
        "held_out.py",
        "exec",
        dont_inherit=True,
    )
    ZlibArchiveWriter(
        str(pyz),
        [("owned.child", "child.py", "PYMODULE"), ("owned", None, "PYMODULE")],
        {"owned.child": unit},
    )
    rows.append(record(pyz.name, pyz.read_bytes(), producer="PyInstaller 6.16.0 PYZ"))
    archive = folder / "owned.pkg"
    CArchiveWriter(
        str(archive),
        [
            ("entry", str(folder / "program.py"), True, "s"),
            ("members.pyz", str(pyz), True, "z"),
            ("owned.so", str(native), True, "b"),
        ],
        "libpython3.10.dylib",
    )
    rows.append(
        record(
            archive.name, archive.read_bytes(), producer="PyInstaller 6.16.0 CArchive"
        )
    )
    missing = folder / "external.pkg"
    CArchiveWriter(
        str(missing), [("library.bin", "companion", False, "d")], "libpython3.10.dylib"
    )
    rows.append(
        record(
            missing.name,
            missing.read_bytes(),
            producer="PyInstaller external dependency",
        )
    )
    return {
        "compiler": sys.version,
        "platform": sys.platform,
        "architecture": platform.machine(),
        "magic": importlib.util.MAGIC_NUMBER.hex(),
        "packager": packager,
        "flags": "exec, dont_inherit=True, on-disk marshal; per-row optimization",
        "native_compiler": subprocess.check_output(
            [clang, "--version"], text=True
        ).splitlines()[0],
        "fixtures": rows,
    }


def verify(cli, folder, manifest):
    for row in manifest["fixtures"]:
        path = folder / row["name"]
        path.write_bytes(base64.b64decode(row["data"], validate=True))
        if hashlib.sha256(path.read_bytes()).hexdigest() != row["sha256"]:
            raise ValueError(path)
        if "disassembly" not in row:
            continue
        report = json.loads(subprocess.check_output([cli, "python", path, "--json"]))
        assert report["status"] == "reconstructed_source", report
        assert report["input_sha256"] == row["sha256"]
        out = folder / (path.name + ".py")
        subprocess.run([cli, "python", path, "-o", out], check=True)
        mode = ["-" + "O" * row["optimize"]] if row["optimize"] else []
        obs = [sys.executable, *mode, str(FIXTURES / "observe.py")]
        expected = subprocess.check_output([*obs, FIXTURES / row["source"]])
        actual = subprocess.check_output([*obs, out])
        if actual != expected:
            raise ValueError((row["name"], expected, actual))
        assert len(report["functions"]) == len(row["disassembly"])
        for fn, expected in zip(report["functions"], row["disassembly"]):
            assert fn["name"] == expected["name"]
            actual = [
                [i["pc"], i["opcode"], i.get("original_line"), i.get("target")]
                for i in fn["instructions"]
            ]
            assert actual == expected["instructions"], (fn["name"], actual, expected)
            for i in fn["instructions"]:
                assert i["offset"] - fn["bytecode_offset"] == i["pc"]
                if "source_line" in i:
                    assert 1 <= i["source_line"] <= len(fn["source"].splitlines())
    print(
        "Owned source behavior, instruction offsets, targets and line mappings match."
    )


def verify_bundle(cli, artifact, folder):
    files = (
        [artifact]
        if artifact.is_file()
        else sorted(
            p for p in artifact.rglob("*") if not p.is_symlink() and p.is_file()
        )
    )

    def identities():
        return [hashlib.sha256(p.read_bytes()).digest() for p in files]

    original = identities()
    inventory = subprocess.run([cli, "python", artifact, "--json"], capture_output=True)
    report = json.loads(inventory.stdout)
    assert report["status"] in ("reconstructed_source", "partial_reconstruction"), (
        report["status"]
    )
    entry = next(
        c
        for c in report["components"]
        if c["name"] == "program" and c.get("member_type") == "s"
    )
    assert entry["root"] == report["root"]
    assert any(c["kind"] == "pyinstaller_pyz" for c in report["components"])
    assert any(c["backend"] == "native" for c in report["components"])
    selected = f"--component={entry['id']}"
    out = folder / (artifact.name + "-recovered.py")
    subprocess.run([cli, "python", artifact, selected, "-o", out], check=True)
    observe = [sys.executable, str(FIXTURES / "observe.py")]
    assert subprocess.check_output(
        [*observe, FIXTURES / "program.py"]
    ) == subprocess.check_output([*observe, out])
    assert identities() == original
    print(
        f"{artifact.name}: {len(report['components'])} components; owned entry behavior and input hashes match."
    )


def build_bundles(cli, folder):
    from PyInstaller import __version__

    if __version__ != "6.16.0":
        raise ValueError("Bundle qualification requires PyInstaller 6.16.0")
    for mode in ("onefile", "onedir"):
        name = "owned-" + mode
        subprocess.run(
            [
                sys.executable,
                "-m",
                "PyInstaller",
                "--" + mode,
                "--name",
                name,
                "--log-level",
                "WARN",
                str(FIXTURES / "program.py"),
            ],
            cwd=folder,
            check=True,
        )
        file = folder / "dist" / name
        if mode == "onefile" and sys.platform == "win32":
            file = file.with_suffix(".exe")
        verify_bundle(cli, file, folder)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--record", action="store_true", help="replace the owned fixture manifest"
    )
    parser.add_argument(
        "--check-producer",
        action="store_true",
        help="regenerate and compare fixture bytes and disassembly",
    )
    parser.add_argument(
        "--neverd",
        type=Path,
        help="verify recovered source against the recorded artifacts",
    )
    parser.add_argument(
        "--bundles",
        action="store_true",
        help="also build and recover full onefile and onedir distributions",
    )
    parser.add_argument("--clang", default="clang")
    args = parser.parse_args()
    if not (args.record or args.check_producer or args.neverd) or (
        args.bundles and not args.neverd
    ):
        parser.error(
            "select --record, --check-producer or --neverd; --bundles requires --neverd"
        )
    if sys.version_info[:3] != (3, 10, 16):
        parser.error("qualification requires CPython 3.10.16")
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        manifest = json.loads(MANIFEST.read_bytes()) if not args.record else None
        if args.record or args.check_producer:
            evidence = generate(folder, args.clang)
            if args.record:
                manifest = evidence
                write_manifest(MANIFEST, manifest)
            else:
                for key in ("fixtures", "magic", "packager", "flags"):
                    if evidence[key] != manifest[key]:
                        raise ValueError(f"Fixture producer mismatch for {key}")
        if args.neverd:
            cli = args.neverd.resolve()
            verify(cli, folder, manifest)
            if args.bundles:
                build_bundles(cli, folder)
