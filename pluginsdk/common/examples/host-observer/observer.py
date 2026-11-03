"""Inspect actual host capabilities and receive an owner-scoped notification."""
import json
from pathlib import Path
import sys

source = Path(__file__).resolve()
if (source.parents[2] / "neverd_host_plugin.py").is_file():
    sys.path.insert(0, str(source.parents[2]))
    sys.path.insert(0, str(source.parents[3] / "gui"))
from neverd_host_plugin import run


def inspect(task):
    services = task.services()
    context = task.service("project.context")
    subscription = task.subscribe("task.changed")
    task.progress("Inspecting host capabilities", 1, 1)
    observed = task.next_event()
    task.unsubscribe(subscription)
    report = dict(services=services, context=context, notification=observed)
    metadata = next((s for s in services["services"] if s["id"] == "analysis.metadata"), None)
    if metadata is not None and metadata["available"]:
        report["metadata"] = task.service("analysis.metadata")
    (task.output_dir / "host-report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    rows = [dict(cells=[s["id"], str(s["version"]), "yes" if s["declared"] else "no",
             "available" if s["available"] else s["unavailable"]["code"]])
            for s in services["services"]]
    return dict(views=[dict(kind="table", title="Host services", columns=[
        "Service", "Version", "Declared", "Availability"], rows=rows),
        dict(kind="text", title="Context and event", text=json.dumps(report, ensure_ascii=False, indent=2))],
        artifacts=[task.artifact("host-report.json", "Host capability report")])


if __name__ == "__main__":
    raise SystemExit(run(inspect))
