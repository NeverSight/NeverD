"""Record or verify the owned CPython 3.11 decoder fixture using stdlib dis."""

import argparse
import base64
import dis
import hashlib
import importlib.util
import json
import platform
import py_compile
import sys
import tempfile
from pathlib import Path

from qualify import FIXTURES, children, write_manifest


def generate(path):
    if sys.version_info[:3] != (3, 11, 11):
        raise ValueError("Regeneration requires CPython 3.11.11")
    source = FIXTURES / "modern.py"
    py_compile.compile(
        str(source),
        str(path),
        dfile=source.name,
        doraise=True,
        optimize=0,
        invalidation_mode=py_compile.PycInvalidationMode.CHECKED_HASH,
    )
    artifact = path.read_bytes()
    code = compile(
        source.read_bytes(), source.name, "exec", dont_inherit=True, optimize=0
    )
    oracle = []
    for child in children(code):
        lines = {
            pc: line
            for start, end, line in child.co_lines()
            for pc in range(start, end, 2)
        }
        insns = list(dis.get_instructions(child, show_caches=False, adaptive=False))
        rows = []
        for index, insn in enumerate(insns):
            end = (
                insns[index + 1].offset
                if index + 1 < len(insns)
                else len(child.co_code)
            )
            target = insn.argval if insn.opcode in dis.hasjrel else None
            rows.append(
                [
                    insn.offset,
                    insn.opname,
                    end - insn.offset,
                    lines[insn.offset],
                    target,
                ]
            )
        oracle.append(
            dict(
                name=child.co_qualname,
                instructions=rows,
                regions=[
                    list(entry) for entry in dis.Bytecode(child).exception_entries
                ],
            )
        )
    return dict(
        compiler=sys.version,
        platform=sys.platform,
        architecture=platform.machine(),
        source=source.name,
        magic=importlib.util.MAGIC_NUMBER.hex(),
        flags="exec, dont_inherit=True, optimize=0, checked hash, on-disk",
        sha256=hashlib.sha256(artifact).hexdigest(),
        data=base64.b64encode(artifact).decode("ascii"),
        functions=oracle,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    manifest = FIXTURES / "modern.json"
    with tempfile.TemporaryDirectory() as folder:
        evidence = generate(Path(folder) / "modern.pyc")
    if args.record:
        write_manifest(manifest, evidence)
    else:
        recorded = json.loads(manifest.read_bytes())
        for key in ("data", "sha256", "magic", "functions", "flags", "source"):
            if recorded[key] != evidence[key]:
                raise ValueError(f"Fixture mismatch for {key}")
        print(
            "CPython 3.11.11 disk bytes, instruction extents, lines and exception regions match."
        )
