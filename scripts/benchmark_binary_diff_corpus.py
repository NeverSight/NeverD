#!/usr/bin/env python3
"""Prepare stripped ELF comparisons with separate, partial DWARF source truth.

This optional benchmark tool needs pyelftools. It does not supply symbols,
function boundaries or source identities to the compared products.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import importlib.metadata
import json
from pathlib import Path
import shutil
import re
import subprocess
import sys

if __package__:
    from . import benchmark_binary_diff as bench
else:
    import benchmark_binary_diff as bench

SPEC_SCHEMA = "neverd.benchmark.diff.sources.v1"
INVENTORY_SCHEMA = "neverd.benchmark.diff.dwarf-bodies.v1"
SHF_ALLOC = 2
SHF_EXECINSTR = 4
CLONE_MARKERS = (".isra.", ".constprop.", ".part.", ".cold", ".llvm.")


def text(value):
    return value.decode("utf-8", errors="strict") if isinstance(value, bytes) else str(value)


def inherited_attribute(die, name):
    """Retain the owning CU for inherited file-index attributes."""
    pending, visited = [(die, ())], set()
    while pending:
        owner, ancestors = pending.pop()
        bench.require(owner.offset not in ancestors, "cyclic DWARF origin/specification")
        if owner.offset in visited:
            continue
        visited.add(owner.offset)
        if name in owner.attributes:
            return owner, owner.attributes[name].value
        for reference in ("DW_AT_specification", "DW_AT_abstract_origin"):
            if reference in owner.attributes:
                pending.append((owner.get_DIE_from_attribute(reference), ancestors + (owner.offset,)))
    return None, None


def source_path(value, directory, root):
    path = Path(text(value))
    path = (Path(directory) / path).resolve()
    return path.relative_to(root).as_posix() if path.is_relative_to(root) else None


def source_identity(die, dwarf, root):
    # GCC LTO places concrete bodies in an artificial compilation unit. The
    # abstract instance retains their original unit, including the distinction
    # between same-named local functions defined by different source files.
    # A specification can name a declaration in another unit, so it must not
    # replace this definition's unit. Missing origin information stays unknown.
    original, visited = die, set()
    while "DW_AT_abstract_origin" in original.attributes:
        bench.require(original.offset not in visited, "cyclic DWARF abstract origin")
        visited.add(original.offset)
        original = original.get_DIE_from_attribute("DW_AT_abstract_origin")
    top = original.cu.get_top_DIE()
    directory = text(top.attributes.get("DW_AT_comp_dir").value) if "DW_AT_comp_dir" in top.attributes else ""
    unit = source_path(top.attributes["DW_AT_name"].value, directory, root)
    owner, file_index = inherited_attribute(die, "DW_AT_decl_file")
    if unit is None or owner is None:
        return None
    line = dwarf.line_program_for_CU(owner.cu)
    if line is None:
        return None
    # This profile deliberately uses DWARF 4. DWARF 5 has different file-index
    # conventions and supplementary references and needs its own qualification.
    bench.require(owner.cu.header.version == 4, "source truth requires DWARF 4")
    bench.require(0 < file_index <= len(line["file_entry"]), "invalid DWARF file index")
    file = line["file_entry"][file_index - 1]
    owning_top = owner.cu.get_top_DIE()
    owning_directory = text(owning_top.attributes["DW_AT_comp_dir"].value) if "DW_AT_comp_dir" in owning_top.attributes else ""
    if file.dir_index:
        bench.require(file.dir_index <= len(line["include_directory"]), "invalid DWARF directory index")
        owning_directory = str(Path(owning_directory) / text(line["include_directory"][file.dir_index - 1]))
    declaration = source_path(file.name, owning_directory, root)
    _, name = inherited_attribute(die, "DW_AT_linkage_name")
    if name is None:
        _, name = inherited_attribute(die, "DW_AT_name")
    if declaration is None or name is None:
        return None
    return (unit, declaration, text(name))


def allocated_sections(elf):
    """Compare runtime sections, allowing strip to rewrite ELF file metadata."""
    return [(s.name, s["sh_type"], s["sh_flags"], s["sh_addr"], s["sh_size"],
             hashlib.sha256(s.data()).hexdigest() if s["sh_type"] != "SHT_NOBITS" else None)
            for s in elf.iter_sections() if s["sh_flags"] & SHF_ALLOC]


def runtime_layout(elf):
    return {"sections": allocated_sections(elf),
            "identity": [elf.elfclass, elf.little_endian, elf["e_type"], elf["e_machine"],
                         elf["e_entry"], elf["e_flags"]],
            "segments": [tuple(segment[key] for key in
                               ("p_type", "p_offset", "p_vaddr", "p_paddr", "p_filesz",
                                "p_memsz", "p_flags", "p_align"))
                         for segment in elf.iter_segments()]}


def overlapping_entries(intervals, entries):
    """Quarantine both ends of overlaps, including entries inside fragments.

    Several records for the same entry do not create a new native identity.
    A sweep of the furthest extent finds every intersecting entry in O(n log n).
    """
    extents = dict.fromkeys(entries, 0)
    for start, end in intervals:
        extents[start] = max(extents.get(start, 0), end)
    overlaps, furthest, owner = set(), 0, None
    for start, end in sorted(extents.items()):
        if start < furthest:
            overlaps.update((owner, start))
        if end > furthest:
            furthest, owner = end, start
    return overlaps


def inspect_elf(binary, root):
    from elftools.elf.elffile import ELFFile
    from elftools.dwarf.descriptions import describe_form_class

    with binary.open("rb") as stream:
        elf = ELFFile(stream)
        bench.require(elf["e_type"] in ("ET_EXEC", "ET_DYN"), "truth requires a linked ELF image")
        bench.require(elf["e_machine"] in ("EM_X86_64", "EM_AARCH64"), "unqualified ELF architecture")
        loads = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        bench.require(bool(loads), "ELF has no load mapping")
        image_base = min(s["p_vaddr"] for s in loads)
        # The current native adapters expose an image base, not a segment map.
        # Qualify only layouts where that base also names the file origin.
        bench.require(any(s["p_vaddr"] == image_base and s["p_offset"] == 0 for s in loads),
                      "source truth requires an offset-zero first load mapping")
        executable_sections = [(s["sh_addr"], s["sh_addr"] + s["sh_size"])
                               for s in elf.iter_sections()
                               if s["sh_flags"] & SHF_ALLOC and s["sh_flags"] & SHF_EXECINSTR]
        executable_loads = [(s["p_vaddr"], s["p_vaddr"] + s["p_filesz"])
                            for s in loads if s["p_flags"] & 1]

        def executable(start, end):
            return (any(lo <= start < end <= hi for lo, hi in executable_sections) and
                    any(lo <= start < end <= hi for lo, hi in executable_loads))

        bench.require(elf.has_dwarf_info(strict=True), "truth binary has no embedded DWARF")
        symbols = elf.get_section_by_name(".symtab")
        bench.require(symbols is not None, "truth requires the original symbol table")
        native, intervals, nonexecutable = defaultdict(set), [], set()
        for symbol in symbols.iter_symbols():
            if symbol["st_info"]["type"] == "STT_FUNC" and symbol["st_shndx"] != "SHN_UNDEF":
                start, size = symbol["st_value"], symbol["st_size"]
                native[start].add(symbol.name)
                section = elf.get_section(symbol["st_shndx"]) if isinstance(symbol["st_shndx"], int) else None
                if (section is None or not section["sh_flags"] & SHF_EXECINSTR or
                        not section["sh_addr"] <= start < start + max(size, 1) <= section["sh_addr"] + section["sh_size"] or
                        not executable(start, start + max(size, 1))):
                    nonexecutable.add(start)
                if size:
                    intervals.append((start, start + size))
        dwarf = elf.get_dwarf_info()
        entries, by_identity = defaultdict(list), defaultdict(set)
        excluded = []
        for unit in dwarf.iter_CUs():
            bench.require(unit.header.version == 4, "source truth requires DWARF 4")
            for die in unit.iter_DIEs():
                if die.tag != "DW_TAG_subprogram":
                    continue
                if "DW_AT_ranges" in die.attributes:
                    identity = source_identity(die, dwarf, root)
                    top = unit.get_top_DIE()
                    base = top.attributes["DW_AT_low_pc"].value if "DW_AT_low_pc" in top.attributes else 0
                    for interval in dwarf.range_lists().get_range_list_at_offset(die.attributes["DW_AT_ranges"].value, cu=unit):
                        if hasattr(interval, "base_address"):
                            base = interval.base_address
                            continue
                        entry = interval.begin_offset + (0 if interval.is_absolute else base)
                        end = interval.end_offset + (0 if interval.is_absolute else base)
                        intervals.append((entry, end))
                        row = {"address": hex(entry), "end": hex(end), "identity": list(identity) if identity else None,
                               "die_offset": die.offset, "reason": "fragmented-body"}
                        entries[entry].append(row)
                        if identity is not None:
                            by_identity[identity].add(entry)
                    continue
                if "DW_AT_low_pc" not in die.attributes:
                    continue
                entry = die.attributes["DW_AT_low_pc"].value
                identity = source_identity(die, dwarf, root)
                row = {"address": hex(entry), "identity": list(identity) if identity else None,
                       "die_offset": die.offset}
                if "DW_AT_high_pc" in die.attributes:
                    high = die.attributes["DW_AT_high_pc"]
                    end = high.value + (entry if describe_form_class(high.form) == "constant" else 0)
                    row["end"] = hex(end)
                    intervals.append((entry, end))
                reason = None
                if identity is None:
                    reason = "no-source-identity"
                elif "DW_AT_high_pc" not in die.attributes:
                    reason = "fragmented-body"
                elif entry not in native:
                    reason = "no-native-function-entry"
                elif entry in nonexecutable or not executable(entry, end):
                    reason = "nonexecutable-body"
                elif len(native[entry]) != 1:
                    reason = "aliased-native-entry"
                elif any(marker in name for name in native[entry] for marker in CLONE_MARKERS):
                    reason = "compiler-clone"
                row["reason"] = reason
                entries[entry].append(row)
                if identity is not None:
                    by_identity[identity].add(entry)
        overlaps = overlapping_entries(intervals, set(native) | set(entries))
        bodies = {}
        for entry, rows in sorted(entries.items()):
            for row in rows:
                identity = tuple(row["identity"]) if row["identity"] else None
                if len(rows) != 1:
                    row["reason"] = "multiple-dwarf-bodies-at-entry"
                elif identity is not None and len(by_identity[identity]) != 1:
                    row["reason"] = "multiple-native-bodies-for-source"
                elif entry in overlaps:
                    row["reason"] = "overlapping-native-body"
                if row["reason"] is not None:
                    excluded.append(row)
                else:
                    bodies[identity] = row
        dynsym = elf.get_section_by_name(".dynsym")
        inventory = {"schema": INVENTORY_SCHEMA, "image_base": hex(image_base),
                     "architecture": elf["e_machine"], "elf_type": elf["e_type"],
                     "native_function_entries": len(native), "dwarf_body_entries": len(entries),
                     "eligible_source_bodies": len(bodies),
                     "excluded_reasons": dict(sorted(Counter(row["reason"] for row in excluded).items())),
                     "excluded": excluded, "bodies": list(bodies.values()),
                     "dynamic_symbol_count": dynsym.num_symbols() if dynsym else 0}
        return bodies, inventory, runtime_layout(elf)


def verify_stripped(original_layout, binary):
    from elftools.elf.elffile import ELFFile

    with binary.open("rb") as stream:
        elf = ELFFile(stream)
        layout = runtime_layout(elf)
        bench.require(layout["sections"] == original_layout["sections"],
                      "stripping changed allocated code/data sections")
        bench.require(layout["identity"] == original_layout["identity"] and
                      layout["segments"] == original_layout["segments"],
                      "stripping changed ELF identity or load mappings")
        for section in elf.iter_sections():
            bench.require(section.name != ".symtab" and
                          not section.name.startswith((".debug", ".zdebug", ".gnu_debug")),
                          "matcher input retains static symbols or debug information")


def prepare(args):
    spec_path = args.spec.resolve()
    spec = bench.load(spec_path)
    bench.require(spec["schema"] == SPEC_SCHEMA, "unsupported source corpus specification")
    bench.require(isinstance(spec["cases"], list) and spec["cases"], "source corpus has no cases")
    identities = set()
    for case in spec["cases"]:
        identity = case["id"]
        bench.require(isinstance(identity, str) and
                      re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", identity) and
                      identity not in identities, "invalid or repeated case identity")
        identities.add(identity)
        for side in ("primary", "secondary"):
            item = case[side]
            bench.checked_artifact(item["binary"], spec_path.parent)
            bench.require(all(isinstance(item[key], str) and item[key] for key in
                              ("project", "revision", "license", "source_url")),
                          "source provenance is incomplete")
            bench.require(item["provenance"], "source/build/license evidence is required")
            for record in item["provenance"]:
                bench.checked_artifact(record, spec_path.parent)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(spec_path, output / "source-spec.json")
    runtime = output / "prepare-runtime"
    runtime.mkdir()
    for path in (Path(__file__), Path(bench.__file__), Path(bench.binexport.__file__), args.strip.resolve()):
        shutil.copy2(path, runtime / path.name)
    bench.save(runtime / "runtime.json", {"python": sys.version,
               "pyelftools": importlib.metadata.version("pyelftools"),
               "files": [bench.artifact(path, runtime) for path in sorted(runtime.iterdir())]})
    manifest = {"schema": bench.PARTIAL_CORPUS_SCHEMA, "cases": [],
                "input_policy": "static symbols and embedded/link debug data removed; dynamic symbols retained",
                "truth_profile": "DWARF-4 unique source-unit/declaration/linkage-name bodies; no semantic claim"}
    for case in spec["cases"]:
        folder = output / case["id"]
        inputs, truth = folder / "inputs", folder / "truth"
        inputs.mkdir(parents=True)
        truth.mkdir()
        prepared = {"id": case["id"], "category": case["category"], "truth_complete": False,
                    "truth_kind": "unique-source-bodies-v1", "truth_artifacts": [], "inventories": {}}
        bodies = []
        for side in ("primary", "secondary"):
            item = case[side]
            original = bench.checked_artifact(item["binary"], spec_path.parent)
            retained = truth / (side + ".debug.elf")
            shutil.copy2(original, retained)
            bench.require(bench.digest(retained) == item["binary"]["sha256"], "truth copy changed")
            root = (spec_path.parent / item["source_root"]).resolve()
            functions, inventory, sections = inspect_elf(retained, root)
            bodies.append(functions)
            inventory["input_provenance"] = {key: item[key] for key in ("project", "revision", "license", "source_url")}
            source_files = sorted({row["identity"][index] for row in functions.values() for index in (0, 1)})
            inventory["source_files"] = [{"path": name, "sha256": bench.digest(root / name)} for name in source_files]
            inventory_path = truth / (side + "-inventory.json")
            bench.save(inventory_path, inventory)
            prepared["inventories"][side] = {key: value for key, value in inventory.items()
                                             if key not in ("excluded", "bodies", "source_files")}
            binary = inputs / (side + ".elf")
            command = [str(args.strip.resolve()), "--strip-all", "-o", str(binary), str(retained)]
            run = subprocess.run(command, capture_output=True, text=True)
            bench.save(truth / (side + "-strip.json"), {"command": command, "exit": run.returncode,
                       "stdout": run.stdout, "stderr": run.stderr})
            bench.require(run.returncode == 0, "strip failed: " + run.stderr)
            verify_stripped(sections, binary)
            prepared[side] = {**bench.artifact(binary, output), "image_base": inventory["image_base"]}
            prepared["truth_artifacts"].extend(bench.artifact(p, output) for p in
                                                (retained, inventory_path, truth / (side + "-strip.json")))
            for index, record in enumerate(item["provenance"]):
                source = bench.checked_artifact(record, spec_path.parent)
                copy = truth / f"{side}-provenance-{index}-{source.name}"
                shutil.copy2(source, copy)
                prepared["truth_artifacts"].append(bench.artifact(copy, output))
        shared = sorted(bodies[0].keys() & bodies[1].keys())
        prepared["truth"] = [{"id": json.dumps(key, ensure_ascii=False, separators=(",", ":")),
                               "primary": bodies[0][key]["address"], "secondary": bodies[1][key]["address"]}
                              for key in shared]
        for side, functions in zip(("primary", "secondary"), bodies):
            prepared["inventories"][side]["paired_source_bodies"] = len(shared)
            prepared["inventories"][side]["unpaired_source_bodies"] = len(functions) - len(shared)
        bench.require(bool(shared), "no qualified common source bodies")
        manifest["cases"].append(prepared)
        print(case["id"], len(shared), "source-body pairs", flush=True)
    bench.corpus_cases(manifest)
    manifest["preparation_artifacts"] = [bench.artifact(path, output) for path in
                                         [output / "source-spec.json", *sorted(runtime.iterdir())]]
    bench.save(output / "manifest.json", manifest)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("spec", "strip", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        return prepare(args)
    except (ValueError, KeyError, TypeError, OSError, ImportError, subprocess.SubprocessError) as error:
        print("source corpus: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
