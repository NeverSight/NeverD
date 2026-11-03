"""Real external providers exercise the native headless host (no Qt)."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HOST = Path(sys.argv[1]).resolve()
SDK = Path(sys.argv[2]).resolve()
del sys.argv[1:]


class ProcessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nph-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "目录 with spaces"
        self.root.mkdir()
        shutil.copy2(SDK / "neverd_gui_plugin.py", self.root)
        self.manifest = self.root / "plugin.json"
        self.declaration = dict(schema="neverd.gui-plugin.v1", id="host-report",
            name="Host report", version="1", program=sys.executable,
            arguments=["provider.py"], actions=[dict(id="run", title="Run", context="none",
                parameters=[dict(id="count", title="Count", type="integer", minimum=1,
                                 maximum=100, default=3)])])
        self.script("""from neverd_gui_plugin import run
import json
def handler(task):
    task.log('report log')
    task.progress('Building report', 1, 1)
    (task.output_dir / 'report.json').write_text(json.dumps(task.parameters), encoding='utf-8')
    return dict(views=[dict(kind='text', title='Count', text=str(task.parameters['count']))],
                artifacts=[task.artifact('report.json', 'Report')])
raise SystemExit(run(handler))
""")

    def script(self, text):
        (self.root / "provider.py").write_text(text, encoding="utf-8")
        self.manifest.write_text(json.dumps(self.declaration), encoding="utf-8")

    def invoke(self, *args, code=0):
        # No Qt search path is needed: this executable only links native NPH.
        result = subprocess.run([str(HOST), "run", str(self.manifest), "run", *args],
                                cwd=self.root, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, code, result.stderr.decode("utf-8", errors="replace"))
        return json.loads(result.stdout), result

    def test_report_receipt_and_effective_configuration(self):
        config = []
        for name, value in (("user", 4), ("profile", 5), ("explicit", 6)):
            path = self.root / (name + ".json")
            path.write_text(json.dumps(dict(count=value)), encoding="utf-8")
            config.append(str(path))
        output = self.root / "result"
        report, result = self.invoke("--user-config", config[0], "--profile", config[1],
            "--parameters", config[2], "--output", str(output))
        self.assertEqual(report["state"], "succeeded")
        self.assertEqual(report["parameters"], dict(count=6))
        self.assertEqual(report, json.loads((output / "receipt.json").read_text(encoding="utf-8")))
        self.assertIn(b"report log", result.stderr)
        artifact = report["result"]["artifacts"][0]
        self.assertEqual(hashlib.sha256((output / artifact["path"]).read_bytes()).hexdigest(), artifact["sha256"])

    def test_defaults_and_invalid_candidate(self):
        report, _ = self.invoke()
        self.assertEqual(report["parameters"], dict(count=3))
        invalid = self.root / "invalid.json"
        invalid.write_text('{"count":null}', encoding="utf-8")
        response, _ = self.invoke("--parameters", str(invalid), code=2)
        self.assertIn("Invalid parameter", response["message"])
        self.assertEqual(invalid.read_text(), '{"count":null}')

    def test_shared_installation_catalog_keeps_intent_and_diagnostics(self):
        catalog = self.root / "catalog.json"

        def manage(command, target=None, code=0):
            args = [str(HOST), "plugins", str(catalog), command]
            if target is not None:
                args.append(str(target))
            result = subprocess.run(args, cwd=self.root, capture_output=True, timeout=8)
            self.assertEqual(result.returncode, code,
                             result.stderr.decode("utf-8", errors="replace"))
            return json.loads(result.stdout)

        installed = manage("import", self.manifest)["plugins"][0]
        self.assertTrue(installed["installed"])
        self.assertTrue(installed["enabled"])
        self.assertEqual(installed["activation"], "active")
        disabled = manage("disable", "host-report")["plugins"][0]
        self.assertFalse(disabled["enabled"])
        self.assertEqual(disabled["activation"], "inactive")
        self.assertTrue(self.manifest.exists())
        self.assertFalse(manage("list")["plugins"][0]["enabled"])
        self.manifest.unlink()
        failed = manage("enable", "host-report", code=1)["plugins"][0]
        self.assertTrue(failed["enabled"])
        self.assertEqual(failed["activation"], "inactive")
        self.assertEqual(failed["discovery_error"]["code"], "manifest_unavailable")
        self.assertTrue(manage("explain", "host-report")["plugins"][0]["enabled"])
        self.assertEqual(manage("remove", "host-report")["plugins"], [])
        self.assertEqual(json.loads(catalog.read_text(encoding="utf-8"))["plugins"], [])
        self.assertTrue((self.root / "provider.py").exists())

    def test_partial_protocol_and_crash_do_not_succeed(self):
        for mode in ("partial-frame", "result-then-crash", "bad-hash", "escaping-artifact",
                     "deep-result"):
            with self.subTest(mode=mode):
                fixture = SDK.parent.parent / "unittests/plugin/process_provider.py"
                self.declaration["arguments"] = [str(fixture), mode]
                # The fixture imports the unchanged helper from this directory.
                shutil.copy2(fixture, self.root / "fixture.py")
                self.declaration["arguments"] = ["fixture.py", mode]
                self.manifest.write_text(json.dumps(self.declaration), encoding="utf-8")
                response, _ = self.invoke(code=1)
                self.assertEqual(response["state"], "failed")
                self.assertEqual(response["result"], {})

    def test_timeout_waits_for_exit(self):
        self.declaration["actions"][0]["timeout_ms"] = 100
        self.script("from neverd_gui_plugin import run\nimport time\n"
                    "def handler(task):\n    time.sleep(10)\n"
                    "raise SystemExit(run(handler))\n")
        report, _ = self.invoke(code=1)
        self.assertEqual(report["state"], "timed_out")

    def test_unicode_protocol_limits_and_binary_stderr(self):
        self.script("""from neverd_gui_plugin import run
import os
def handler(task):
    os.write(2, b'\\xff')
    task.progress('中' * 1024)
    task.log('中' * 8192)
    return dict(views=[])
raise SystemExit(run(handler))
""")
        report, _ = self.invoke()
        self.assertEqual(report["state"], "succeeded")
        self.assertIn('\ufffd', report["stderr"])
        # Truncation must never split a UTF-8 character inside the JSON receipt.
        self.script("""from neverd_gui_plugin import read_frame, write_frame
import sys
write_frame(sys.stdout.buffer, dict(type='hello'))
request = read_frame(sys.stdin.buffer)
write_frame(sys.stdout.buffer, dict(type='error', task_id=request['task_id'], message='中' * 8193))
""")
        report, _ = self.invoke(code=1)
        self.assertEqual(report["state"], "failed")
        self.assertEqual(report["diagnostic"], '中' * 8192)

    def test_child_tree_is_stopped_before_success(self):
        self.script("""from neverd_gui_plugin import run
import subprocess, sys
def handler(task):
    marker = str(task.output_dir / 'survived')
    source = "import time;from pathlib import Path;time.sleep(1);Path(" + repr(marker) + ").write_text('bad')"
    subprocess.Popen([sys.executable, '-c', source], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return dict(views=[])
raise SystemExit(run(handler))
""")
        output = self.root / "tree"
        self.invoke("--output", str(output))
        import time
        time.sleep(1.2)
        self.assertFalse((output / "survived").exists())

    def test_headless_action_context_is_explained(self):
        self.declaration["actions"][0]["context"] = "function"
        self.manifest.write_text(json.dumps(self.declaration), encoding="utf-8")
        report, _ = self.invoke(code=2)
        self.assertIn("context is unavailable", report["message"])

    def test_generic_services_and_events_without_an_engine(self):
        self.declaration["services"] = [dict(id="project.context", version=1),
                                        dict(id="project.context", version=99)]
        self.declaration["events"] = [dict(id="task.changed", version=1)]
        self.script("""from neverd_gui_plugin import run, QueryError
import json
def handler(task):
    catalog = task.services()
    assert catalog['missing'] == [dict(id='project.context', version=99, code='service_unavailable')]
    context = task.service('project.context')
    assert context == task.context and not context['loaded']
    try: task._host('services.call', registration='unknown', payload={})
    except QueryError as error: assert error.code == 'service_undeclared'
    else: raise AssertionError('forged binding accepted')
    subscription = task.subscribe('task.changed')
    task.progress('Event roundtrip', 1, 1)
    event = task.next_event()
    assert event['event']['payload']['task_id'] == task.id
    assert event['event']['payload']['progress']['label'] == 'Event roundtrip'
    task.unsubscribe(subscription)
    task.unsubscribe(subscription)
    (task.output_dir / 'event.json').write_text(json.dumps(event), encoding='utf-8')
    return dict(views=[], artifacts=[task.artifact('event.json', 'Event')])
raise SystemExit(run(handler))
""")
        report, _ = self.invoke()
        self.assertEqual(report["state"], "succeeded")
        self.assertEqual(len(report["result"]["artifacts"]), 1)

    def test_invalid_capability_declarations(self):
        for declaration in ([dict(id="project.context", version=True)],
                            [dict(id="project.context", version=0)],
                            [dict(id="project.context", version=1)] * 2):
            with self.subTest(declaration=declaration):
                self.declaration["services"] = declaration
                self.manifest.write_text(json.dumps(self.declaration), encoding="utf-8")
                report, _ = self.invoke(code=2)
                self.assertIn("plugin services requirement", report["message"])

    def test_prepared_host_observer_example(self):
        source = SDK.parent / "common/examples/host-observer"
        package = self.root / "standalone host observer"
        subprocess.run([sys.executable, str(source / "prepare.py"), str(package)],
                       check=True, capture_output=True)
        result = subprocess.run([str(HOST), "run", str(package / "plugin.json"), "inspect",
                                 "--output", str(self.root / "observer-result")],
                                capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))
        data = json.loads((self.root / "observer-result/host-report.json").read_text(encoding="utf-8"))
        self.assertFalse(data["context"]["loaded"])
        self.assertEqual(data["notification"]["event"]["id"], "task.changed")
        self.assertEqual(data["services"]["missing"][0]["id"], "analysis.metadata")


if __name__ == "__main__":
    unittest.main()
