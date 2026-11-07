"""Consumer transport, restart identity, cancellation and catalog boundaries."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

host = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "pluginsdk/common/examples/tool-roundtrip"))
from caller import Consumer

with tempfile.TemporaryDirectory(prefix="tools-consumer-") as temp:
    directory = Path(temp)
    provider = directory / "provider.py"
    provider.write_text(
        "import sys, time\n"
        f"sys.path.insert(0, {str(root / 'pluginsdk/gui')!r})\n"
        "from neverd_gui_plugin import run\n"
        "def action(task):\n"
        "    task.progress('Started')\n"
        "    if task.action == 'wait': time.sleep(30)\n"
        "    else:\n"
        "        assert task.service('project.context') == task.context\n"
        "        subscription = task.subscribe('task.changed')\n"
        "        task.progress('Shared services/events')\n"
        "        assert task.next_event()['event']['payload']['task_id'] == task.id\n"
        "        task.unsubscribe(subscription)\n"
        "    return dict(views=[], data=task.parameters['value'] ** 2)\n"
        "sys.exit(run(action))\n", encoding="utf-8")
    parameters = [dict(id="value", title="Value", type="integer", default=7, minimum=1, maximum=10)]
    manifest = dict(schema="neverd.gui-plugin.v1", id="consumer-test", name="Consumer fixture", version="1",
                    program=sys.executable, arguments=[str(provider)], queries=[],
                    services=[dict(id="project.context", version=1)],
                    events=[dict(id="task.changed", version=1)],
                    actions=[dict(id=name, title=name, context="none", parameters=parameters)
                             for name in ("square", "wait")])
    catalog = dict(schema="neverd.tools.v1", tools=[
        dict(id=name, action=name, description=name, effect="read", output_schema=dict(type="integer"))
        for name in ("square", "wait")])
    (directory / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
    (directory / "tools.json").write_text(json.dumps(catalog), encoding="utf-8")

    def command(output):
        return [str(host), "tools", str(directory / "plugin.json"), str(directory / "tools.json"),
                "--output", str(directory / output)]

    caller = Consumer(command("first"))
    try:
        # Malformed JSON is a request error, not process termination.
        caller.process.stdin.write("{invalid\n")
        caller.process.stdin.flush()
        assert caller.read()["error"]["code"] == "invalid_request"
        installed = caller.request("plugins")["plugins"]
        assert len(installed) == 1 and installed[0]["id"] == "consumer-test"
        assert installed[0]["installed"] and installed[0]["enabled"]
        assert installed[0]["activation"] == "active"
        initial = caller.request("resolve", session="consumer", step="one")
        tools = {t["id"]: t for t in initial["tools"]}
        result = caller.request("call", set=initial["set"], registration=tools["square"]["registration"], call_id="square")
        operation = result["value"]["operation"]
        assert caller.done(operation)["data"] == 49
        old_runtime = caller.ready["runtime"]
    finally:
        caller.close()

    caller = Consumer(command("restart"))
    try:
        assert caller.ready["runtime"] != old_runtime
        caller.process.stdin.write(json.dumps(dict(id="stale", runtime=old_runtime, op="resolve", session="consumer", step="one")) + "\n")
        caller.process.stdin.flush()
        assert caller.read()["error"]["code"] == "runtime_changed"
        assert caller.request("inspect", operation=operation)["error"]["code"] == "outcome_unknown"
        current = caller.request("resolve", session="consumer", step="one")
        slow = next(t for t in current["tools"] if t["id"] == "wait")
        result = caller.request("call", set=current["set"], registration=slow["registration"], call_id="wait")
        pending = result["value"]["operation"]
        caller.request("cancel", operation=pending)
        assert caller.done(pending)["state"] == "cancelled"
        result = caller.request("call", set=current["set"], registration=slow["registration"], call_id="disconnect")
        disconnected = result["value"]["operation"]
    finally:
        caller.close()
    receipt = json.loads((directory / "restart" / disconnected / "receipt.json").read_text(encoding="utf-8"))
    assert receipt["state"] == "cancelled", receipt

    # Registration of a later catalog item must reject the whole component.
    catalog["tools"].append(dict(id="third", action="missing", description="Invalid", effect="read", output_schema=dict(type="integer")))
    (directory / "tools.json").write_text(json.dumps(catalog), encoding="utf-8")
    rejected = subprocess.run(command("invalid"), capture_output=True, timeout=10)
    assert rejected.returncode == 2, rejected.stdout
    assert json.loads(rejected.stdout)["status"] == "error"
    print("Tools consumer passed: malformed input, runtime identity, call cancellation, disconnect receipt, atomic catalog failure")
