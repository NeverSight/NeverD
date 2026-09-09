"""Cloud trigger identity mutations. REST responses are self-authored mocks."""
from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
import os
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from scripts import mobile_real_apps_trigger as trigger


ENV = {"GITHUB_ACTIONS": "true", "GITHUB_EVENT_NAME": "workflow_run",
       "GITHUB_SERVER_URL": "https://github.com", "GITHUB_API_URL": "https://api.github.com",
       "GITHUB_REPOSITORY": "Owned/Application", "GITHUB_REPOSITORY_ID": "123",
       "GH_TOKEN": "test-credential-must-not-appear-in-output"}
REPO = {"id": 123, "full_name": "Owned/Application", "default_branch": "dev"}
WORKFLOW = {"id": 352466821, "name": "Mobile Decompilation", "path": ".github/workflows/mobile.yml"}
RUN = {"id": 456, "run_attempt": 2, "workflow_id": 352466821,
       "name": WORKFLOW["name"], "path": WORKFLOW["path"], "head_sha": "a" * 40,
       "head_branch": "dev", "event": "push", "status": "completed", "conclusion": "success",
       "repository": REPO, "head_repository": REPO}
EVENT = {"action": "completed", "repository": REPO, "workflow_run": RUN}


class TriggerTests(unittest.TestCase):
    def verify(self, event=None, *, repository=None, workflow=None, run=None, env=None):
        values = {"/repos/Owned/Application": deepcopy(REPO if repository is None else repository),
                  "/repos/Owned/Application/actions/workflows/352466821": deepcopy(WORKFLOW if workflow is None else workflow),
                  "/repos/Owned/Application/actions/runs/456/attempts/2": deepcopy(RUN if run is None else run)}
        calls = []

        def get(path):
            calls.append(path)
            self.assertIn(path, values, "Only the fixed repository, workflow and event attempt may be read")
            return values[path]

        result = trigger.verify_event(deepcopy(EVENT if event is None else event), ENV if env is None else env, get)
        self.assertEqual(set(calls), set(values))
        return result

    def test_exact_attempt_is_bound_without_using_the_latest_rerun(self):
        result = self.verify()
        self.assertEqual(result["consumer_commit"], "a" * 40)
        self.assertEqual(result["producer_run_id"], 456)
        self.assertEqual(result["producer_run_attempt"], 2)
        self.assertEqual(result["producer_conclusion"], "success")

    def test_failed_producer_still_has_valid_identity_and_retains_its_conclusion(self):
        for conclusion in ("failure", "cancelled", "timed_out", "skipped", "action_required"):
            with self.subTest(conclusion=conclusion):
                event, run = deepcopy(EVENT), deepcopy(RUN)
                event["workflow_run"]["conclusion"] = run["conclusion"] = conclusion
                self.assertEqual(self.verify(event, run=run)["producer_conclusion"], conclusion)

    def test_only_trusted_actions_environment_and_github_dot_com_are_accepted(self):
        mutations = {"GITHUB_ACTIONS": "false", "GITHUB_EVENT_NAME": "push", "GITHUB_SERVER_URL": "https://other.example",
                     "GITHUB_API_URL": "https://other.example", "GITHUB_REPOSITORY_ID": "true",
                     "GITHUB_REPOSITORY": "Owned/Application\nextra=value"}
        for key, value in mutations.items():
            with self.subTest(key=key), self.assertRaises(trigger.TriggerError):
                self.verify(env={**ENV, key: value})
        for key in mutations:
            env = dict(ENV)
            env.pop(key)
            with self.subTest(missing=key), self.assertRaises(trigger.TriggerError):
                self.verify(env=env)

    def test_event_rejects_pr_forks_nondev_uncompleted_or_injected_metadata(self):
        mutations = {"id": True, "run_attempt": "2", "workflow_id": False,
                     "head_sha": "a" * 40 + "\nextra=value", "head_branch": "main",
                     "event": "pull_request", "status": "in_progress", "conclusion": None,
                     "name": "Other", "path": ".github/workflows/other.yml",
                     "head_repository": {"id": 321, "full_name": "Fork/Application"}}
        for key, value in mutations.items():
            event = deepcopy(EVENT)
            event["workflow_run"][key] = value
            with self.subTest(key=key), self.assertRaises(trigger.TriggerError):
                self.verify(event)
        for key in ("action", "repository", "workflow_run"):
            event = deepcopy(EVENT)
            event.pop(key)
            with self.subTest(missing=key), self.assertRaises(trigger.TriggerError):
                self.verify(event)

    def test_repository_workflow_and_each_rest_attempt_identity_must_agree(self):
        for key, value in (("id", True), ("id", 999), ("full_name", "Other/Application"), ("default_branch", "main")):
            with self.subTest(repository=key), self.assertRaises(trigger.TriggerError):
                self.verify(repository={**REPO, key: value})
        for key, value in (("id", 123), ("path", ".github/workflows/other.yml"), ("name", "Other")):
            with self.subTest(workflow=key), self.assertRaises(trigger.TriggerError):
                self.verify(workflow={**WORKFLOW, key: value})
        for key, value in (("id", 789), ("run_attempt", 3), ("head_sha", "b" * 40), ("conclusion", "failure"),
                           ("event", "workflow_dispatch"), ("status", "queued"), ("workflow_id", 999),
                           ("repository", {"id": 999, "full_name": REPO["full_name"]})):
            with self.subTest(attempt=key), self.assertRaises(trigger.TriggerError):
                self.verify(run={**RUN, key: value})

    def test_explicit_workflow_dispatch_is_a_valid_producer_event(self):
        event, run = deepcopy(EVENT), deepcopy(RUN)
        event["workflow_run"]["event"] = run["event"] = "workflow_dispatch"
        self.verify(event, run=run)

    def test_missing_scalar_and_noninteger_fields_never_coerce_into_a_valid_identity(self):
        for key in ("id", "run_attempt", "workflow_id", "head_sha", "head_repository", "status", "conclusion"):
            event = deepcopy(EVENT)
            event["workflow_run"].pop(key)
            with self.subTest(key=key), self.assertRaises(trigger.TriggerError):
                self.verify(event)
        for value in (True, False, 0, -1, 1.5, "456", None, 2 ** 64):
            event = deepcopy(EVENT)
            event["workflow_run"]["id"] = value
            with self.subTest(value=value), self.assertRaises(trigger.TriggerError):
                self.verify(event)

    def test_json_duplicate_fields_nonfinite_values_and_size_overflow_are_rejected(self):
        for data in (b'{"id":1,"id":2}', b'{"value":NaN}', b'[]', b'{bad', b' ' * (trigger.MAX_JSON_BYTES + 1)):
            with self.subTest(data=data[:32]), self.assertRaises(trigger.TriggerError):
                trigger.decode_json(data)

    def test_cli_publishes_only_four_validated_values_and_no_credentials(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            event, output, report = root / "event.json", root / "github-output", root / "report.json"
            event.write_text(json.dumps(EVENT))
            result = self.verify()
            stdout = io.StringIO()
            with patch.dict(os.environ, ENV, clear=True), patch.object(trigger, "GitHubAPI"), \
                    patch.object(trigger, "verify_event", return_value=result), redirect_stdout(stdout):
                self.assertEqual(trigger.main(["--event", str(event), "--github-output", str(output), "--output", str(report)]), 0)
            outputs = dict(line.split("=", 1) for line in output.read_text().splitlines())
            self.assertEqual(outputs, {key: str(result[key]) for key in trigger.OUTPUT_KEYS})
            self.assertEqual(json.loads(report.read_text()), result)
            self.assertNotIn(ENV["GH_TOKEN"], stdout.getvalue() + output.read_text() + report.read_text())

    def test_invalid_event_does_not_publish_any_outputs_or_diagnostics_containing_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            event, output, report = root / "event.json", root / "github-output", root / "report.json"
            event.write_text('{"bad":"' + ENV["GH_TOKEN"] + '"}')
            stderr = io.StringIO()
            with patch.dict(os.environ, ENV, clear=True), patch.object(trigger, "GitHubAPI"), redirect_stderr(stderr):
                self.assertEqual(trigger.main(["--event", str(event), "--github-output", str(output), "--output", str(report)]), 1)
            self.assertFalse(output.exists())
            self.assertFalse(report.exists())
            self.assertNotIn(ENV["GH_TOKEN"], stderr.getvalue())

    def test_last_remote_check_failure_cannot_publish_a_partially_verified_source(self):
        for failure in ("sha-mismatch", "transport"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                event, output, report = root / "event.json", root / "github-output", root / "report.json"
                event.write_text(json.dumps(EVENT))
                attempts = []

                def get(path):
                    attempts.append(path)
                    if path == "/repos/Owned/Application":
                        return deepcopy(REPO)
                    if path == "/repos/Owned/Application/actions/workflows/352466821":
                        return deepcopy(WORKFLOW)
                    self.assertEqual(path, "/repos/Owned/Application/actions/runs/456/attempts/2")
                    if failure == "transport":
                        raise trigger.TriggerError("GitHub metadata request failed")
                    return {**deepcopy(RUN), "head_sha": "b" * 40}

                stderr = io.StringIO()
                with patch.dict(os.environ, ENV, clear=True), \
                        patch.object(trigger, "GitHubAPI", return_value=get), redirect_stderr(stderr):
                    result = trigger.main(["--event", str(event), "--github-output", str(output), "--output", str(report)])
                self.assertEqual(result, 1)
                self.assertEqual(len(attempts), 3)
                self.assertFalse(output.exists())
                self.assertFalse(report.exists())
                self.assertNotIn(ENV["GH_TOKEN"], stderr.getvalue())

    def test_transport_is_get_only_bounded_and_never_follows_redirects(self):
        response = io.BytesIO(json.dumps(REPO).encode())
        response.status = 200
        with patch.object(trigger, "build_opener") as build:
            build.return_value.open.return_value = response
            api = trigger.GitHubAPI(ENV["GH_TOKEN"])
            self.assertEqual(api("/repos/Owned/Application"), REPO)
            handlers = build.call_args.args
            redirect = next(handler for handler in handlers if isinstance(handler, trigger.NoRedirect))
            self.assertIsNone(redirect.redirect_request(None, None, 302, "redirect", {}, "https://other.example"))
            request = build.return_value.open.call_args.args[0]
            self.assertEqual(request.get_method(), "GET")
            self.assertEqual(request.full_url, "https://api.github.com/repos/Owned/Application")
            self.assertIsNone(request.data)
            self.assertEqual(build.return_value.open.call_args.kwargs["timeout"], 30)

    def test_transport_errors_oversize_and_duplicate_json_are_sanitized(self):
        for body in (b'{"id":1,"id":2}', b'x' * (trigger.MAX_JSON_BYTES + 1)):
            response = io.BytesIO(body)
            response.status = 200
            with patch.object(trigger, "build_opener") as build:
                build.return_value.open.return_value = response
                with self.assertRaises(trigger.TriggerError):
                    trigger.GitHubAPI(ENV["GH_TOKEN"])("/repos/Owned/Application")
        with patch.object(trigger, "build_opener") as build:
            build.return_value.open.side_effect = HTTPError("https://api.github.com/", 403, ENV["GH_TOKEN"], {}, None)
            with self.assertRaises(trigger.TriggerError) as caught:
                trigger.GitHubAPI(ENV["GH_TOKEN"])("/repos/Owned/Application")
            self.assertNotIn(ENV["GH_TOKEN"], str(caught.exception))


class WorkflowContractTests(unittest.TestCase):
    def workflow(self):
        return (Path(__file__).resolve().parents[2] / ".github/workflows/mobile-real-apps.yml").read_text()

    def assert_contract(self, text):
        active = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        events = re.search(r"(?ms)^on:\n(.*?)(?=^\S)", active)
        self.assertIsNotNone(events)
        self.assertEqual(re.findall(r"(?m)^  ([\w-]+):", events[1]), ["workflow_run"])
        self.assertIn("workflows: [Mobile Decompilation]", events[1])
        self.assertIn("types: [completed]", events[1])
        self.assertIn("branches: [dev]", events[1])
        self.assertNotRegex(active, r"(?i)uses:\s*[^\n]*(?:cache@|cache/|sccache)")
        self.assertNotRegex(active, r"(?im)^\s*(?:cache|SCCACHE_[A-Z_]+):")
        self.assertNotIn("sccache", active.lower())
        self.assertNotRegex(active, r"(?m)^\s*[\w-]+:\s*(?:write|write-all)\s*$")
        jobs = dict(re.findall(r"(?ms)^  ([\w-]+):\n(.*?)(?=^  \S|\Z)", active.split("\njobs:\n", 1)[1]))
        self.assertEqual(set(jobs), {"trigger", "producer", "guards", "build", "android", "ios", "release-gate"})
        for name, body in jobs.items():
            if name == "trigger":
                self.assertIn("actions: read", body)
                self.assertIn("GH_TOKEN: ${{ github.token }}", body)
            else:
                needs = re.search(r"(?m)^    needs:\s*([^\n]+)", body)
                self.assertIsNotNone(needs)
                self.assertIn("trigger", re.findall(r"[\w-]+", needs[1]))
                condition = re.search(r"(?m)^    if:\s*([^\n]+)", body)
                if condition:
                    self.assertIn("needs.trigger.result == 'success'", condition[1])
                    self.assertNotIn("||", condition[1])
                self.assertNotIn("actions: read", body)
                self.assertNotRegex(body, r"GH_TOKEN|github\.token|secrets\.")
            for checkout in re.findall(r"(?ms)^      - uses: actions/checkout@[^\n]*\n(.*?)(?=^      - |\Z)", body):
                self.assertIn("persist-credentials: false", checkout)
                if not re.search(r"(?m)^          repository:", checkout):
                    ref = "github.sha" if name == "trigger" else "needs.trigger.outputs.consumer_commit"
                    self.assertIn("ref: ${{ " + ref + " }}", checkout)
            self.assertNotRegex(body, r"(?m)^\s*(?:run-id|github-token):")
        self.assertIn("PRODUCER_CONCLUSION: ${{ needs.trigger.outputs.producer_conclusion }}", jobs["producer"])
        self.assertIn('test "$PRODUCER_CONCLUSION" = success', jobs["producer"])
        gate_needs = re.search(r"(?m)^    needs:\s*([^\n]+)", jobs["release-gate"])
        self.assertEqual(set(re.findall(r"[\w-]+", gate_needs[1])), {"trigger", "producer", "guards", "build", "android", "ios"})
        self.assertIn("needs.trigger.result == 'success'", jobs["release-gate"])

    def test_actual_workflow_preserves_read_only_trigger_and_same_consumer_identity(self):
        self.assert_contract(self.workflow())

    def test_cache_write_trigger_untrusted_checkout_and_gate_bypass_mutations_fail(self):
        text = self.workflow()
        mutations = [text.replace("  workflow_run:\n", "  push:\n  workflow_run:\n", 1),
                     text.replace("contents: read", "contents: write", 1),
                     text.replace("ref: ${{ needs.trigger.outputs.consumer_commit }}", "ref: ${{ github.sha }}", 1),
                     text.replace("needs: trigger", "needs: []", 1),
                     text.replace("always() && needs.trigger.result == 'success'", "always()", 1),
                     text.replace("uses: actions/setup-python@", "uses: actions/cache@", 1),
                     text.replace("test \"$PRODUCER_CONCLUSION\" = success", "test true = true", 1)]
        for index, changed in enumerate(mutations):
            self.assertNotEqual(changed, text)
            with self.subTest(mutation=index), self.assertRaises(AssertionError):
                self.assert_contract(changed)


if __name__ == "__main__":
    unittest.main()
