"""Deterministic Tools consumer: inspect, select, follow up, verify. No model."""
import argparse
import json
from pathlib import Path
import subprocess
import time


class Consumer:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        text=True, encoding="utf-8")
        self.serial = 0
        self.events = []
        self.ready = self.read()
        if self.ready.get("event") != "ready":
            raise RuntimeError(self.ready)

    def read(self):
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"Tools host exited: {self.process.poll()}")
        return json.loads(line)

    def request(self, op, **fields):
        self.serial += 1
        request_id = str(self.serial)
        self.process.stdin.write(json.dumps(dict(id=request_id, runtime=self.ready["runtime"], op=op, **fields)) + "\n")
        self.process.stdin.flush()
        while True:
            reply = self.read()
            if reply.get("id") == request_id:
                return reply
            self.events.append(reply)

    def done(self, operation):
        while True:
            reply = self.request("inspect", operation=operation)
            if reply["status"] != "ok":
                raise RuntimeError(reply)
            value = reply["value"]
            if value["state"] not in {"running", "cancelling"}:
                return value
            time.sleep(0.02)

    def close(self):
        self.process.stdin.close()
        self.process.stdout.read()
        self.process.stdout.close()
        if self.process.wait(timeout=15):
            raise RuntimeError("Tools host did not close successfully")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=Path)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--function", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", help="Explicit reviewed name; default is the report suggestion")
    args = parser.parse_args()
    package = Path(__file__).resolve().parent
    consumer = Consumer([str(args.host), "tools", str(package / "plugin.json"), str(package / "tools.json"),
                         "--worker", str(args.worker), "--input", str(args.input), "--function", args.function,
                         "--output", str(args.output), "--allow-project-writes"])
    try:
        step = consumer.request("resolve", session="example", step="review")
        tools = {item["id"]: item for item in step["tools"]}
        first = consumer.request("call", set=step["set"], registration=tools["report"]["registration"],
                                 call_id="report", parameters={})
        report = consumer.done(first["value"]["operation"])
        if report["state"] != "succeeded":
            raise RuntimeError(report)
        selection = args.name or report["data"]["suggested_name"]
        second = consumer.request("call", set=step["set"], registration=tools["apply-name"]["registration"],
                                  call_id="apply", parameters={"name": selection}, parent_operation=report["operation"])
        applied = consumer.done(second["value"]["operation"])
        if applied["state"] != "succeeded" or not applied["data"]["saved"]:
            raise RuntimeError(applied)
        fresh = consumer.request("resolve", session="example", step="verify")
        report_tool = next(t for t in fresh["tools"] if t["id"] == "report")
        third = consumer.request("call", set=fresh["set"], registration=report_tool["registration"],
                                 call_id="verify", parameters={})
        verified = consumer.done(third["value"]["operation"])
        if verified["state"] != "succeeded" or verified["data"]["current_name"] != selection:
            raise RuntimeError(verified)
        print(json.dumps(dict(report=report["data"], applied=applied["data"],
                              receipts=applied["receipts"], verified=verified["data"]), indent=2))
    finally:
        consumer.close()


if __name__ == "__main__":
    main()
