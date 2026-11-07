"""An independent useful Tools plugin, without TVM, Qt or a model dependency."""
import json
import sys

from neverd_tools_plugin import Task, run


def action(task: Task):
    if task.action == "report":
        entry = task.context["function_address"]
        task.progress("Inspecting entry instructions", 0, 1)
        identity = task.query("resolve", query=entry)
        instructions = task.query("disasm", address=entry, limit=task.parameters["limit"])["items"]
        if not instructions:
            raise RuntimeError("No instructions at the selected entry")
        calls = sum(row["mnemonic"].lower() in {"call", "callq", "bl", "blr"} for row in instructions)
        report = dict(address=entry, instruction_count=len(instructions), calls=calls,
                      suggested_name=f"reviewed_{int(entry, 16):x}", current_name=identity.get("name", ""))
        (task.output_dir / "instructions.json").write_text(
            json.dumps(dict(report=report, instructions=instructions), ensure_ascii=False, indent=2), encoding="utf-8")
        task.progress("Report ready for review", 1, 1)
        return dict(data=report, views=[dict(kind="text", title="Entry report", text=json.dumps(report, indent=2))],
                    artifacts=[task.artifact("instructions.json", "Inspected instructions")])
    if task.action == "apply-name":
        if not task.continuation:
            raise RuntimeError("Invoke this tool with a report operation through Tools")
        report = task.continuation["data"]
        task.progress("Submitting reviewed name")
        receipt = task.command("rename", address=report["address"], name=task.parameters["name"])
        result = dict(address=report["address"], name=task.parameters["name"],
                      saved=receipt["payload"]["saved"], revision=receipt["revision"])
        return dict(data=result, views=[dict(kind="text", title="Committed name", text=result["name"])])
    raise ValueError(f"Unknown action {task.action}")


if __name__ == "__main__":
    sys.exit(run(action))
