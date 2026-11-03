"""Function reports and reusable GUI plugin examples, independent of TVM."""
from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import sys
import time

# Support both the SDK source manifest and a prepared, self-contained copy.
_here = Path(__file__).resolve().parent
for _directory in (_here, _here.parent.parent):
    if (_directory / "neverd_gui_plugin.py").is_file():
        sys.path.insert(0, str(_directory))
        break

from neverd_gui_plugin import QueryError, Task, run


def number(value) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def optional(task: Task, notes: list[str], operation: str, **payload):
    try:
        return task.query(operation, **payload)
    except QueryError as error:
        if (error.status != "budget_exceeded" and error.code not in
                {"unsupported", "backend_error", "engine_error", "analysis_failed", "budget_exceeded"}):
            raise
        note = f"{operation}: {error}"
        if note not in notes:
            notes.append(note)
        return None


def instruction_kind(mnemonic: str) -> str:
    mnemonic = mnemonic.lower()
    if mnemonic in {"call", "callq", "bl", "blr"}:
        return "call"
    if mnemonic in {"jmp", "jmpq", "b", "br"}:
        return "jump"
    if mnemonic.startswith(("j", "loop", "b.")) or mnemonic in {"cbz", "cbnz", "tbz", "tbnz"}:
        return "conditional branch"
    if mnemonic in {"ret", "retq", "retn", "retf"}:
        return "return"
    return "instruction"


def linear_instructions(task: Task, entry: str, size: int, notes: list[str]):
    """Scan only the ND function extent, or one explicitly bounded window."""
    start = number(entry)
    end = start + size if size > 0 else None
    limit = 512 if end is not None else 64
    cursor = start
    items = []
    continuous = True
    while len(items) < limit:
        page = task.query("disasm", address=hex(cursor), limit=min(128, limit - len(items)))
        before = cursor
        for item in page.get("items", []):
            address = number(item["address"])
            width = number(item["size"])
            if address < cursor or width <= 0:
                raise ValueError("Disassembly page did not advance monotonically")
            if end is not None and address >= end:
                return items, continuous and cursor >= end
            if end is not None and address + width > end:
                notes.append("The last decoded instruction crosses ND's declared function boundary; it was excluded.")
                return items, False
            if address > cursor:
                continuous = False
                notes.append(f"No instruction was returned for the byte range {hex(cursor)}..{hex(address)}.")
            items.append(item)
            cursor = address + width
        if end is not None and cursor >= end:
            return items, continuous
        if cursor == before or page.get("complete"):
            break
    if end is None:
        notes.append("ND did not publish a function size; the report covers only the first 64 linear instructions.")
    elif cursor < end:
        notes.append(f"Linear listing stopped at {hex(cursor)}: at most 512 instructions are inspected.")
    return items, False


def function_report(task: Task, include_graph: bool = False) -> dict:
    entry = task.context["function_address"]
    notes = []
    task.progress("Reading function identity")
    found = optional(task, notes, "functions", filter=entry[2:], offset=0, limit=128)
    function = next((row for row in (found or {}).get("items", [])
                     if number(row["address"]) == number(entry)), {})
    name = function.get("name", f"Function at {entry}")
    size = number(function.get("size", 0))
    task.progress("Reading function instructions")
    instructions, complete = linear_instructions(task, entry, size, notes)
    if not instructions:
        raise RuntimeError(f"No instructions could be decoded at {entry}")

    task.progress("Resolving call sites and references")
    imported = optional(task, notes, "imports", offset=0, limit=512)
    imports = {number(row["address"]): row for row in (imported or {}).get("items", [])}
    if imported and not imported.get("complete", True):
        notes.append("Import naming uses the first 512 import slots; remaining targets keep their addresses.")
    names = {}
    calls = []
    references = []
    queried = 0
    for instruction in instructions:
        kind = instruction_kind(instruction.get("mnemonic", ""))
        operands = instruction.get("operands", "")
        address = instruction["address"]
        reference_rows = []
        if (kind == "call" or "[" in operands or "rip" in operands.lower()) and queried < 32:
            queried += 1
            response = optional(task, notes, "xrefs", address=address, direction="from", limit=32)
            reference_rows = (response or {}).get("items", [])
            if response and not response.get("complete", True):
                notes.append(f"Reference page for {address} is partial (32 entries).")
        call_targets = []
        for reference in reference_rows:
            target = reference.get("to")
            if target is None:
                continue
            target_address = number(target)
            imported_symbol = imports.get(target_address)
            reference_kind = reference.get("kind", reference.get("type", "reference"))
            label = reference.get("text", "")
            if imported_symbol:
                label = imported_symbol.get("module", "") + "!" + imported_symbol.get("name", "")
            references.append(dict(cells=[address, str(reference_kind), hex(target_address), label], address=address))
            if kind == "call" and (reference_kind in {"call", "p"} or imported_symbol):
                if imported_symbol:
                    evidence = "import slot; runtime callee address not inferred"
                else:
                    if target_address not in names and len(names) < 16:
                        identity = optional(task, notes, "resolve", query=hex(target_address))
                        names[target_address] = (identity or {}).get("name", "")
                    label = names.get(target_address) or label or hex(target_address)
                    evidence = "direct reference from ND"
                call_targets.append((hex(target_address), label, evidence))
        if kind == "call":
            if not call_targets:
                call_targets.append((operands, "unresolved", "operand retained; no target guessed"))
            for target, label, evidence in call_targets:
                calls.append(dict(cells=[address, target, label, evidence], address=address))
    if queried == 32:
        notes.append("Reference lookup is limited to 32 instruction sites; unqueried operands remain visible in the listing.")
    if len(references) > 512:
        notes.append(f"Reference table shows 512 of {len(references)} observations; JSON keeps all observations.")

    kinds = [instruction_kind(row.get("mnemonic", "")) for row in instructions]
    summary = [name, f"Entry: {entry}", f"ND function size: {size if size else 'unknown'} bytes",
               f"Decoded linear instructions: {len(instructions)}",
               f"Linear byte-range coverage: {'complete' if complete else 'bounded / partial'}",
               f"Call instructions: {kinds.count('call')}",
               f"Conditional branches: {kinds.count('conditional branch')}",
               f"Unconditional branches: {kinds.count('jump')}",
               f"Return instructions: {kinds.count('return')}",
               f"Reference sites queried: {queried}",
               "", "Instruction counts describe the linear listing, not a proof of reachable execution paths."]
    views = [dict(kind="text", title="Function summary", text=""),
             dict(kind="table", title="Calls", columns=["Call site", "Target / slot", "Name", "Evidence"], rows=calls[:512]),
             dict(kind="table", title="References", columns=["Instruction", "Kind", "Target / slot", "Name / detail"], rows=references[:512]),
             dict(kind="table", title="Instructions", columns=["Address", "Mnemonic", "Operands"],
                  rows=[dict(cells=[row["address"], row.get("mnemonic", ""), row.get("operands", "")], address=row["address"]) for row in instructions])]
    cfg = None
    if include_graph:
        task.progress("Reading control-flow graph")
        cfg = optional(task, notes, "cfg", address=entry)
        if cfg:
            nodes = cfg.get("nodes", [])
            edges = cfg.get("edges", [])
            summary.extend([f"", f"CFG: {len(nodes)} blocks, {len(edges)} edges"])
            shown = nodes[:128]
            ids = {str(node["id"]) for node in shown}
            shown_edges = [{"from": str(edge["from"]), "to": str(edge["to"])}
                           for edge in edges if str(edge.get("from")) in ids and str(edge.get("to")) in ids]
            views.append(dict(kind="graph", title=f"CFG preview ({len(shown)}/{len(nodes)} blocks)",
                              nodes=[dict(id=str(node["id"]), text=str(node.get("label", node["id"])),
                                          address=node.get("address", node["start"])) for node in shown], edges=shown_edges[:512]))
            if len(nodes) > 128 or len(shown_edges) > 512:
                notes.append("The graph is a bounded preview. The exported JSON contains the full CFG returned by ND.")
    if notes:
        summary.extend(["", "Coverage and unavailable optional information:", *["- " + note for note in dict.fromkeys(notes)]])
    views[0]["text"] = "\n".join(summary)
    data = dict(schema="workbench.function-report.v1", entry=entry, name=name, size=size,
                linear_coverage_complete=complete, instructions=instructions, calls=calls,
                references=references, notes=list(dict.fromkeys(notes)), cfg=cfg)
    (task.output_dir / "function-report.json").write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    (task.output_dir / "function-report.md").write_text("\n".join(summary) + "\n", encoding="utf-8")
    task.progress("Function report ready", 1, 1)
    return dict(views=views, artifacts=[task.artifact("function-report.json", "Function report (JSON)"),
                                      task.artifact("function-report.md", "Function summary (Markdown)")])


def handle(task: Task) -> dict:
    task.log(f"Running {task.action}; captured epoch={task.context.get('epoch')}")
    if task.action == "report":
        return function_report(task)
    if task.action == "context":
        return {"views": [dict(kind="text", title="Captured context",
                               text=json.dumps(task.context, ensure_ascii=False, indent=2))]}

    if task.action == "overview":
        task.progress("Reading project metadata")
        metadata = task.query("metadata")
        functions = task.query("functions", offset=0, limit=512, sort="size", descending=True)
        rows = [dict(cells=[item.get("name", ""), item["address"], str(item.get("size", ""))],
                     address=item["address"]) for item in functions.get("items", [])]
        task.progress("Function catalogue ready", 1, 1)
        return {"views": [dict(kind="table", title="Largest functions",
                               columns=["Name", "Address", "Size"], rows=rows),
                          dict(kind="text", title="Scope",
                               text=f"Image: {metadata.get('path', task.context.get('input_path', ''))}\n"
                                    f"Architecture: {metadata.get('architecture', '')}\n"
                                    f"Showing {len(rows)} of {functions.get('total', 'unknown')} functions, ranked by size.\n"
                                    "Activate a row to inspect that function. This is a catalogue ranking; no function bodies are analyzed.")]}

    if task.action == "instructions":
        address = task.context["address"]
        instructions = task.query("disasm", address=address, limit=32)
        memory = task.query("bytes", address=address, size=64)
        rows = [dict(cells=[item["address"], item.get("mnemonic", ""), item.get("operands", "")],
                     address=item["address"]) for item in instructions.get("items", [])]
        return {"views": [dict(kind="table", title="Instructions",
                               columns=["Address", "Mnemonic", "Operands"], rows=rows),
                          dict(kind="text", title="Bytes and selection",
                               text=f"Address: {address}\nBytes read: {memory.get('bytes_read', 0)}\n"
                                    f"Mapping: {memory.get('mapping_status', 'unknown')}\n"
                                    f"Hex: {memory.get('data', '')}\n\n" + task.context.get("selection_text", ""))]}

    if task.action == "graph":
        return function_report(task, include_graph=True)

    if task.action == "image-code":
        metadata = task.query("metadata")
        if (metadata.get("bitness") != 64 or
                not any(name in metadata.get("architecture", "").lower()
                        for name in ("86", "x64", "amd64"))):
            raise ValueError("This analysis-section example emits x64 instructions")
        if not metadata.get("analysis_sections_supported"):
            raise ValueError("The loaded ND engine does not support analysis sections")
        address = int(task.parameters["address"], 0)
        if not 0 < address < 1 << 64:
            raise ValueError("Invalid section address")
        value = task.parameters["value"]
        code = b"\xb8" + value.to_bytes(4, "little") + b"\xc3"
        (task.output_dir / "returns-value.bin").write_bytes(code)
        suffix = task.id.replace("-", "")[:12]
        description = dict(schema="neverd.analysis-section.v1", name=f".example_{suffix}",
                           address=hex(address), permissions="rx", functions=[
                               dict(name=f"example_returns_{value}_{suffix}", offset="0x0", size=len(code))])
        return dict(views=[dict(kind="text", title="Analysis function",
                               text=f"Prepared an x64 function returning {value} at {address:#x}.\n"
                                    "Use Import code into image, then the normal CFG and F5 views.\n"
                                    "This is example-generated code; it is not recovered from the input.")],
                    artifacts=[task.artifact("returns-value.bin", "Example x64 function")],
                    image_addition=dict(artifact=0, description=description,
                        sample_sha256=hashlib.sha256(Path(task.context["input_path"]).read_bytes()).hexdigest()))

    if task.action == "background":
        parameters = task.parameters
        steps = parameters["steps"]
        task.progress("Preparing demonstration work")  # Unknown total.
        for index in range(steps):
            time.sleep(parameters["delay_ms"] / 1000)
            task.progress("Demonstration work", index + 1, steps)
            task.log(f"Completed step {index + 1}/{steps}")
        message = parameters["message"]
        if parameters["letter_case"] == "upper":
            message = message.upper()
        report = dict(message=message, steps=steps, context=task.context,
                      note="Demonstration work only; no binary transformation or execution")
        artifacts = []
        if parameters["write_report"]:
            (task.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
            artifacts.append(task.artifact("report.json", "Demonstration report"))
        return {"views": [dict(kind="text", title="Work result", text=json.dumps(report, ensure_ascii=False, indent=2))],
                "artifacts": artifacts}

    if task.action == "failure":
        mode = task.parameters["mode"]
        if mode == "crash":
            os._exit(17)
        if mode == "timeout":
            time.sleep(30)
        if mode == "malformed":
            return {"views": [{"kind": "unsupported", "title": "Invalid view"}]}
        raise RuntimeError("Intentional example failure; inspect the task state and diagnostics")

    raise ValueError(f"Unknown example action: {task.action}")


if __name__ == "__main__":
    raise SystemExit(run(handle))
