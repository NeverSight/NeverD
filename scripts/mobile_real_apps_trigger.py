#!/usr/bin/env python3
"""Bind a cloud-only workflow_run consumer to its exact trusted producer attempt.

Only read-only GitHub REST metadata is fetched; no producer artifact or cache
is downloaded. A failed producer remains a valid identity, not a passing gate.
https://docs.github.com/en/rest/actions/workflow-runs#get-a-workflow-run-attempt
"""
from __future__ import annotations

import argparse
from http.client import HTTPException
import json
import os
from pathlib import Path
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener


MAX_JSON_BYTES = 2 * 1024 * 1024
WORKFLOW_ID = 352466821
WORKFLOW_NAME = "Mobile Decompilation"
WORKFLOW_PATH = ".github/workflows/mobile.yml"
OUTPUT_KEYS = ("consumer_commit", "producer_run_id", "producer_run_attempt", "producer_conclusion")
CONCLUSIONS = {"success", "failure", "neutral", "cancelled", "skipped", "timed_out",
               "action_required", "stale", "startup_failure"}


class TriggerError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise TriggerError(message)


def object_value(value, label):
    require(isinstance(value, dict), label + " must be an object")
    return value


def positive_id(value, label):
    require(type(value) is int and 0 < value < 2 ** 63, label + " must be a positive integer ID")
    return value


def decode_json(data):
    require(isinstance(data, bytes) and len(data) <= MAX_JSON_BYTES, "JSON metadata exceeds the byte limit")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "JSON metadata contains a duplicate field")
            result[key] = value
        return result

    def invalid_constant(_):
        raise TriggerError("JSON metadata contains a nonfinite number")

    try:
        value = json.loads(data, object_pairs_hook=unique, parse_constant=invalid_constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise TriggerError("JSON metadata is invalid or exceeds parser limits") from None
    return object_value(value, "JSON metadata")


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # A token-bearing request must never be redirected to another origin.
        return None


class GitHubAPI:
    def __init__(self, token):
        require(isinstance(token, str) and 0 < len(token) <= 4096
                and all(33 <= ord(char) <= 126 for char in token), "Missing or invalid read-only GitHub credential")
        self.token = token
        self.opener = build_opener(NoRedirect())

    def __call__(self, path):
        require(isinstance(path, str) and re.fullmatch(r"/repos/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_-]+)*", path),
                "Invalid GitHub metadata endpoint")
        request = Request("https://api.github.com" + path, method="GET", headers={
            "Accept": "application/vnd.github+json", "Authorization": "Bearer " + self.token,
            "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "NeverD-mobile-trigger"})
        try:
            with self.opener.open(request, timeout=30) as response:
                require(response.status == 200, "GitHub metadata request did not return HTTP 200")
                data = response.read(MAX_JSON_BYTES + 1)
        except HTTPError as error:
            # Do not serialize an HTTP body, URL, header, or exception message.
            raise TriggerError(f"GitHub metadata request failed with HTTP {error.code}") from None
        except (URLError, OSError, HTTPException):
            raise TriggerError("GitHub metadata request failed; no source identity was published") from None
        return decode_json(data)


def verify_event(event, environ, api_get):
    require(environ.get("GITHUB_ACTIONS") == "true" and environ.get("GITHUB_EVENT_NAME") == "workflow_run",
            "Only a GitHub Actions workflow_run event is accepted")
    require(environ.get("GITHUB_SERVER_URL") == "https://github.com"
            and environ.get("GITHUB_API_URL") == "https://api.github.com", "Only github.com Actions metadata is accepted")
    repository = environ.get("GITHUB_REPOSITORY")
    require(isinstance(repository, str) and re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository),
            "Missing or invalid trusted repository name")
    repository_id = environ.get("GITHUB_REPOSITORY_ID")
    require(isinstance(repository_id, str) and re.fullmatch(r"[1-9][0-9]{0,18}", repository_id),
            "Missing or invalid trusted repository ID")
    repository_id = positive_id(int(repository_id), "Trusted repository")

    def same_repository(value):
        value = object_value(value, "Repository identity")
        require(positive_id(value.get("id"), "Repository") == repository_id
                and value.get("full_name") == repository, "Repository identity differs from the trusted consumer repository")

    def workflow_identity(value, *, run=False):
        value = object_value(value, "Workflow identity")
        require(positive_id(value.get("workflow_id" if run else "id"), "Workflow") == WORKFLOW_ID
                and value.get("name") == WORKFLOW_NAME and value.get("path") == WORKFLOW_PATH,
                "Producer workflow ID, path or name differs from the fixed trusted workflow")

    def run_identity(value):
        value = object_value(value, "Producer run")
        positive_id(value.get("id"), "Producer run")
        positive_id(value.get("run_attempt"), "Producer attempt")
        workflow_identity(value, run=True)
        same_repository(value.get("head_repository"))
        require(isinstance(value.get("head_sha"), str) and re.fullmatch(r"[0-9a-f]{40}", value["head_sha"]),
                "Producer head SHA is missing or invalid")
        require(value.get("head_branch") == "dev" and value.get("event") in ("push", "workflow_dispatch"),
                "Producer must be a dev push or explicit dispatch in the same repository")
        require(value.get("status") == "completed" and isinstance(value.get("conclusion"), str)
                and value["conclusion"] in CONCLUSIONS, "Producer attempt is not completed with a recognized conclusion")
        return value

    event = object_value(event, "Actions event")
    require(event.get("action") == "completed", "Only completed producer events are accepted")
    same_repository(event.get("repository"))
    emitted = run_identity(event.get("workflow_run"))
    # Some event payloads have only the top-level repository. If also present
    # in workflow_run, it must not contradict that identity.
    if "repository" in emitted:
        same_repository(emitted["repository"])
    if "workflow" in event:
        workflow_identity(event["workflow"])

    prefix = "/repos/" + repository
    current_repository = object_value(api_get(prefix), "REST repository")
    same_repository(current_repository)
    require(current_repository.get("default_branch") == "dev", "Trusted repository default branch must remain dev")
    workflow_identity(api_get(prefix + f"/actions/workflows/{WORKFLOW_ID}"))
    # Query the emitted attempt, never /runs/<id>, whose current attempt may
    # already have changed when an older completion event is delivered.
    observed = run_identity(api_get(prefix + f"/actions/runs/{emitted['id']}/attempts/{emitted['run_attempt']}"))
    same_repository(observed.get("repository"))
    fields = ("id", "run_attempt", "workflow_id", "name", "path", "head_sha", "head_branch", "event", "status", "conclusion")
    require(all(observed[key] == emitted[key] for key in fields), "REST attempt differs from the completed event identity")
    return {"schema_version": 1, "status": "verified", "repository": repository, "repository_id": repository_id,
            "consumer_commit": observed["head_sha"], "producer_run_id": observed["id"],
            "producer_run_attempt": observed["run_attempt"], "producer_conclusion": observed["conclusion"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", type=Path, required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        with args.event.open("rb") as stream:
            event = decode_json(stream.read(MAX_JSON_BYTES + 1))
        result = verify_event(event, os.environ, GitHubAPI(os.environ.get("GH_TOKEN")))
        # Invalid metadata publishes neither a report nor any job outputs.
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        with args.github_output.open("a", encoding="utf-8") as stream:
            stream.write("".join(f"{key}={result[key]}\n" for key in OUTPUT_KEYS))
        print("Verified the exact dev producer attempt; its conclusion is retained for the separate producer gate.")
        return 0
    except TriggerError as error:
        # TriggerError messages contain fixed diagnostics, never input values,
        # HTTP bodies, credentials or nested exceptions. Keep the failed check
        # visible so a cloud failure can be diagnosed without dumping payloads.
        print(f"error: {error}", file=sys.stderr)
        return 1
    except OSError:
        print("error: cannot read or publish GitHub producer identity metadata", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
