"""Generate disk opcode assignments from unpacked, stable CPython source releases."""

import argparse
import ast
from collections import defaultdict
from pathlib import Path
import re

HERE = Path(__file__).resolve().parents[2]
DESTINATION = HERE / "lib/analysis/bytecode/python/ReleaseOpcodes.def"
DEFINITION = re.compile(
    r"""^(def_op|name_op|jrel_op|jabs_op)\(\s*['"]([^'"]+)['"]\s*,\s*(\d+)""",
    re.MULTILINE,
)


def literal(node):
    if isinstance(node, ast.Dict):
        return {
            literal(key): literal(value) for key, value in zip(node.keys, node.values)
        }
    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id in {"dict", "frozendict"}
    ):
        return {key.arg: literal(key.value) for key in node.keywords}
    return ast.literal_eval(node)


def assignment(source, name):
    for statement in ast.parse(source).body:
        if isinstance(statement, ast.Assign):
            targets = statement.targets
            if (
                len(targets) == 1
                and isinstance(targets[0], ast.Name)
                and targets[0].id == name
            ):
                return literal(statement.value)
    raise ValueError(f"Missing data assignment {name}")


def classic_opcodes(root, source, release):
    opcodes = {}
    domains = {}
    jumps = {
        "def_op": "",
        "name_op": "Name",
        "jrel_op": "Forward",
        "jabs_op": "Absolute",
    }
    for kind, name, number in DEFINITION.findall(source):
        opcodes[name] = int(number)
        domains[int(number)] = jumps[kind]
    indices = {
        "const": "Constant",
        "name": "Name",
        "local": "Local",
        "free": "Free",
        "compare": "Compare",
    }
    for kind, number in re.findall(
        r"^has(const|name|local|free|compare)\.append\((\d+)\)", source, re.MULTILINE
    ):
        domains[int(number)] = indices[kind]
    if release <= 104:
        # Early dis modules lag the VM. The actual opcode header owns assignments.
        header = (root / "Include/opcode.h").read_text()
        active = {
            name: int(number)
            for name, number in re.findall(
                r"^#define\s+([A-Z][A-Z_0-9]*)\s+(\d+)\b", header, re.MULTILINE
            )
            if name != "HAVE_ARGUMENT" and int(number) < 256
        }
        opcodes = {
            name: num for name, num in opcodes.items() if name.split("+")[0] in active
        }
        for name, number in active.items():
            if (
                name in {"SLICE", "STORE_SLICE", "DELETE_SLICE"}
                or opcodes.get(name) == number
            ):
                continue
            opcodes = {old: num for old, num in opcodes.items() if num != number}
            opcodes[name] = number
            domains[number] = ""
        if "UNPACK_VARARG" in opcodes:
            domains[opcodes["UNPACK_VARARG"]] = ""
    first_argument = int(
        re.search(r"^HAVE_ARGUMENT\s*=\s*(\d+)", source, re.MULTILINE)[1]
    )
    return (
        opcodes,
        domains,
        {number for number in opcodes.values() if number >= first_argument},
    )


def modern_opcodes(root):
    opcodes = assignment((root / "Lib/_opcode_metadata.py").read_text(), "opmap")
    header = (root / "Include/internal/pycore_opcode_metadata.h").read_text()
    domains = {}
    arguments = set()
    for name, flags in re.findall(
        r"\[(\w+)\]\s*=\s*\{\s*true,\s*INSTR_FMT_[^,]+,\s*([^}]+)", header
    ):
        if name not in opcodes:
            continue
        number = opcodes[name]
        if "HAS_ARG_FLAG" in flags:
            arguments.add(number)
        for flag, domain in (
            ("JUMP", "Forward"),
            ("CONST", "Constant"),
            ("NAME", "Name"),
            ("LOCAL", "Local"),
            ("FREE", "Free"),
        ):
            if f"HAS_{flag}_FLAG" in flags:
                domains[number] = domain
                break
    return opcodes, domains, arguments


def operand(name, domain, release):
    if name in {"LOAD_FAST", "STORE_FAST", "DELETE_FAST"} and release >= 103:
        return "Local"
    if name in {
        "LOAD_FAST_BORROW_LOAD_FAST_BORROW",
        "LOAD_FAST_LOAD_FAST",
        "STORE_FAST_LOAD_FAST",
        "STORE_FAST_STORE_FAST",
    }:
        return "LocalPair"
    if (
        name == "LOAD_GLOBAL"
        and release >= 311
        or name == "LOAD_ATTR"
        and release >= 312
    ):
        return "NameFlag"
    if name == "LOAD_SUPER_ATTR" or name == "IMPORT_NAME" and release >= 315:
        return "PackedName"
    if name == "COMPARE_OP":
        return "Compare"
    if name in {"LOAD_SMALL_INT", "LOAD_COMMON_CONSTANT"}:
        return "Immediate"
    if name == "EXTENDED_ARG":
        return "Prefix"
    if domain == "Forward" and "BACKWARD" in name:
        return "Backward"
    return domain


def transfer(name):
    if name in {
        "RETURN_VALUE",
        "RETURN_CONST",
        "RETURN_NONE",
        "RAISE_VARARGS",
        "RERAISE",
        "STOP_CODE",
        "RAISE_EXCEPTION",
    }:
        return "Stop"
    if name in {
        "JUMP_FORWARD",
        "JUMP_BACKWARD",
        "JUMP_BACKWARD_NO_INTERRUPT",
        "JUMP_ABSOLUTE",
        "CONTINUE_LOOP",
    }:
        return "Jump"
    if name in {"SETUP_EXCEPT", "SETUP_FINALLY", "SETUP_WITH", "SETUP_ASYNC_WITH"}:
        return "Handler"
    if name in {
        "SETUP_LOOP",
        "BREAK_LOOP",
        "END_FINALLY",
        "END_ASYNC_FOR",
        "WITH_CLEANUP",
        "END_FOR_ITER",
        "RETURN_GENERATOR",
    }:
        return "Dynamic"
    return "Next"


def disk_layout(root, release):
    module = root / "Lib/opcode.py"
    source = (module if module.exists() else root / "Lib/dis.py").read_text()
    if release >= 313:
        opcodes, domains, arguments = modern_opcodes(root)
    else:
        opcodes, domains, arguments = classic_opcodes(root, source, release)
    caches = {}
    if release >= 312:
        caches = {
            name: sum(fields.values())
            for name, fields in assignment(source, "_cache_format").items()
        }
    runtime_only = {
        "CACHE",
        "RESERVED",
        "ENTER_EXECUTOR",
        "INTERPRETER_EXIT",
        "BINARY_OP_INPLACE_ADD_UNICODE",
        "COPY_FREE_VARS_PLACEHOLDER",
    }
    for name, number in opcodes.items():
        if (
            number >= 256
            or name.startswith(("INSTRUMENTED_", "<"))
            or name in runtime_only
        ):
            continue
        domain = domains.get(number) or ("Immediate" if number in arguments else "None")
        yield (
            number,
            name,
            operand(name, domain, release),
            transfer(name),
            caches.get(name, 0),
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "sources",
        nargs="+",
        metavar="RELEASE=DIR",
        help="One unpacked CPython release per disk layout (3.10 and 3.11 have dedicated decoders)",
    )
    parser.add_argument("--output", type=Path, default=DESTINATION)
    args = parser.parse_args()
    profiles = {}
    for item in args.sources:
        version, root = item.split("=", 1)
        parts = [int(n) for n in version.split(".")]
        release = parts[-1] if parts[0] == 0 else parts[0] * 100 + parts[1]
        if release in profiles:
            parser.error(f"Duplicate layout for {version}")
        if release not in (310, 311):
            profiles[release] = list(disk_layout(Path(root), release))
    catalogue = (HERE / "include/neverd/analysis/PythonVersions.h").read_text()
    expected = {int(v) for v in re.findall(r'\{\s*(\d+),\s*"', catalogue)} - {310, 311}
    if set(profiles) != expected:
        parser.error(
            f"Missing or unrecognized release IDs: {sorted(expected ^ set(profiles))}"
        )
    order = sorted(profiles)
    ranges = defaultdict(list)
    for index, release in enumerate(order):
        for fact in profiles[release]:
            intervals = ranges[fact]
            if intervals and intervals[-1][1] == index - 1:
                intervals[-1][1] = index
            else:
                intervals.append([index, index])
    rows = sorted(
        (number, order[a], order[b], *fields)
        for (number, *fields), intervals in ranges.items()
        for a, b in intervals
    )
    header = DESTINATION.read_text().split("PYTHON_RELEASE_OPCODE(", 1)[0]
    args.output.write_text(
        header
        + "".join(
            f'PYTHON_RELEASE_OPCODE({first}, {last}, {number}, "{name}", {domain}, {flow}, {cache})\n'
            for number, first, last, name, domain, flow, cache in rows
        )
    )


if __name__ == "__main__":
    main()
