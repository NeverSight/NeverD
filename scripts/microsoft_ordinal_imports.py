"""Match short COFF ordinal imports to externally witnessed Windows export names."""
from __future__ import annotations

from functools import cache
import json
from pathlib import Path
import re
import struct
import sys


INPUTS = Path(__file__).with_name("MicrosoftOrdinalSDKInputs.json")
MACHINES = {"x86": 0x014c, "x64": 0x8664, "arm64": 0xaa64}
ARCHIVE_MEMBER_SIZE = 60
IMPORT_OBJECT_HEADER = struct.Struct("<HHHHIIHH")


@cache
def profiles():
    return json.loads(INPUTS.read_text(encoding="utf-8"))["providers"]


def witness(profile, architecture):
    target = profile["targets"][architecture]
    return {"module": profile["module"], "image_sha256": target["image_sha256"],
            "exports": {**profile["exports"], **target["additional_exports"]}}


def members(archive):
    """Yield the bounded payloads of a Microsoft COFF archive."""
    if archive[:8] != b"!<arch>\n":
        raise ValueError("missing COFF archive signature")
    offset = 8
    while offset < len(archive):
        header = archive[offset:offset + ARCHIVE_MEMBER_SIZE]
        if len(header) != ARCHIVE_MEMBER_SIZE or header[58:] != b"`\n":
            raise ValueError("malformed COFF archive member header")
        length = header[48:58].strip()
        if not length.isdigit():
            raise ValueError("invalid COFF archive member size")
        start = offset + ARCHIVE_MEMBER_SIZE
        end = start + int(length)
        offset = end + (end & 1)
        if offset > len(archive):
            raise ValueError("truncated COFF archive member")
        yield archive[start:end]


def symbols(path, architecture, module, exports):
    """Keep the linker spellings that reach a witnessed code ordinal on this target."""
    by_ordinal = {ordinal: name for name, ordinal in exports.items()}
    if not by_ordinal or len(by_ordinal) != len(exports) or any(
            type(ordinal) is not int or not 1 <= ordinal <= 0xffff for ordinal in by_ordinal):
        raise ValueError("invalid or ambiguous witnessed export ordinal")
    target = MACHINES[architecture]
    expected_module = module.encode("ascii").lower()
    result = {}
    for data in members(path.read_bytes()):
        if not data.startswith(b"\0\0\xff\xff"):
            continue
        if len(data) < IMPORT_OBJECT_HEADER.size:
            raise ValueError("truncated COFF short import header")
        _, _, version, machine, _, size, ordinal, bits = IMPORT_OBJECT_HEADER.unpack_from(data)
        # Versioned bigobj records share the signature. For short imports zero
        # flags mean executable code imported by ordinal, including clear reserved bits.
        if version or machine != target or bits or ordinal not in by_ordinal:
            continue
        strings = data[IMPORT_OBJECT_HEADER.size:]
        if size != len(strings):
            raise ValueError("invalid ordinal import string table size")
        fields = strings.split(b"\0")
        if len(fields) != 3 or fields[-1] or not fields[0] or not fields[1]:
            raise ValueError("invalid ordinal import symbol or DLL name")
        if fields[1].lower() == expected_module:
            result.setdefault(by_ordinal[ordinal], set()).add(fields[0].decode("ascii"))
    if missing := exports.keys() - result.keys():
        raise ValueError(f"{module} lacks witnessed SDK code ordinals for " + ", ".join(sorted(missing)))
    return result


def loader_table(inputs):
    """One sorted row per module and ordinal, with independent target names."""
    table = {}
    for profile in [*inputs["providers"].values(), *inputs["existing_loader_contracts"]]:
        module = profile["module"]
        if not re.fullmatch(r"[a-z0-9_.-]+\.dll", module):
            raise ValueError("invalid Windows export module: " + module)
        for arch in profile["targets"]:
            if arch not in MACHINES:
                raise ValueError("unsupported Windows ordinal architecture: " + arch)
            for name, number in witness(profile, arch)["exports"].items():
                if (not re.fullmatch(r"[A-Za-z_]\w*", name, re.ASCII) or type(number) is not int
                        or not 1 <= number <= 0xffff):
                    raise ValueError(f"invalid {module} export identity for {arch}")
                names = table.setdefault((module, number), {})
                if names.setdefault(arch, name) != name:
                    raise ValueError(f"conflicting {module} ordinal {number} for {arch}")
    lines = []
    for (module, number), names in sorted(table.items()):
        targets = ", ".join(json.dumps(names[arch]) if arch in names else "nullptr" for arch in MACHINES)
        lines.append(f'  {{"{module}", {number}, {{{targets}}}}},')
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(loader_table(json.loads(INPUTS.read_text(encoding="utf-8"))), encoding="utf-8")
