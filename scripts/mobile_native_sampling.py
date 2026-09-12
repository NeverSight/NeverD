"""Actions-only statistical stack evidence for slow native iOS workers."""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import time

SAMPLE_DELAYS = (60.0, 140.0)
MAX_REPORT_BYTES = 16 * 1024 * 1024
MAX_PROCESS_BYTES = 1024 * 1024
PS_COMMAND = ("/bin/ps", "-axo", "pid=,ppid=,comm=")


def owned_worker(table: str, leader: int, executable: str):
    """Return the unique matching descendant and its complete observed lineage."""
    if len(table.encode("utf-8")) > MAX_PROCESS_BYTES:
        raise ValueError("Process observation exceeds its parsing budget")
    rows = {}
    for line in table.splitlines():
        fields = line.split(None, 2)
        if len(fields) != 3 or not fields[0].isdecimal() or not fields[1].isdecimal():
            raise ValueError("Malformed process observation")
        pid, parent = int(fields[0]), int(fields[1])
        if pid <= 0 or pid in rows:
            raise ValueError("Ambiguous process observation")
        rows[pid] = (parent, fields[2])
    if leader not in rows or Path(rows[leader][1]).name != executable:
        raise ValueError("Native command leader is absent or changed")
    matches = []
    for pid, (_, command) in rows.items():
        if pid == leader or Path(command).name != executable:
            continue
        chain, seen, current = [], set(), pid
        while current in rows and current not in seen and len(chain) < 32:
            seen.add(current)
            parent, name = rows[current]
            chain.append((current, parent, name))
            if current == leader:
                matches.append((pid, tuple(chain)))
                break
            current = parent
    if len(matches) != 1:
        raise ValueError("No unique native worker belongs to this command")
    return matches[0]


class NativeWorkerSampler:
    @classmethod
    def create(cls, variant, command, neverd, environment, directory, stem, started):
        if sys.platform != "darwin" or variant.get("platform") != "ios" \
                or command[:2] != [str(neverd), "mobile"] \
                or environment.get("NEVERD_NATIVE_PHASES") != "1":
            return None
        return cls(Path(directory), stem, Path(neverd).name, dict(environment), started)

    def __init__(self, directory, stem, executable, environment, started):
        self.directory, self.stem, self.executable = directory, stem, executable
        self.environment, self.started = environment, started
        self.records = []

    @staticmethod
    def remaining(deadline, cap):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Native sample attempt exhausted its deadline")
        return min(cap, remaining)

    def observe(self, leader, deadline):
        result = subprocess.run(PS_COMMAND, stdin=subprocess.DEVNULL, capture_output=True,
                                env=self.environment, timeout=self.remaining(deadline, 1),
                                check=False)
        if result.returncode or len(result.stdout) > MAX_PROCESS_BYTES:
            raise ValueError("Native worker process observation is unavailable")
        return owned_worker(result.stdout.decode("utf-8", errors="strict"),
                            leader, self.executable)

    def file_receipt(self, path):
        if path.is_symlink() or not path.is_file():
            return {"path": path.relative_to(self.directory.parent).as_posix(),
                    "status": "missing-or-unsafe"}
        size = path.stat().st_size
        receipt = {"path": path.relative_to(self.directory.parent).as_posix(), "bytes": size}
        if size > MAX_REPORT_BYTES:
            receipt["status"] = "oversized"
            return receipt
        with path.open("rb") as stream:
            data = stream.read(MAX_REPORT_BYTES + 1)
        if len(data) != size:
            receipt["status"] = "changed"
            return receipt
        receipt.update(status="retained", sha256=hashlib.sha256(data).hexdigest())
        return receipt

    def capture_if_due(self, leader, now, command_deadline):
        index = len(self.records)
        if index >= len(SAMPLE_DELAYS) or now - self.started < SAMPLE_DELAYS[index]:
            return False
        record = {"schema_version": 1, "scope": "statistical-native-worker-stack",
                  "requested_after_seconds": SAMPLE_DELAYS[index],
                  "started_after_seconds": now - self.started, "leader_pid": leader,
                  "status": "unavailable", "native_exit_status_overridden": False,
                  "may_perturb_timing": True}
        self.records.append(record)
        if command_deadline - now < 8:
            record.update(status="skipped", reason="Insufficient original command time remains")
            return True
        directory = self.directory / f"{self.stem}-native-sample-{index}"
        report = directory / "sample.txt"
        stdout, stderr = directory / "tool.stdout", directory / "tool.stderr"
        attempt_deadline = min(command_deadline, now + 5)
        try:
            directory.mkdir()
            worker, lineage = self.observe(leader, attempt_deadline)
            if self.observe(leader, attempt_deadline) != (worker, lineage):
                raise ValueError("Native worker ownership changed before sampling")
            record.update(worker_pid=worker, ancestry_pids=[row[0] for row in lineage],
                          duration_seconds=1, interval_milliseconds=10)
            command = ["/usr/bin/sample", str(worker), "1", "10", "-mayDie", "-file", str(report)]
            record["argv"] = command
            with stdout.open("xb") as out, stderr.open("xb") as err:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                        env=self.environment, check=False,
                                        timeout=self.remaining(attempt_deadline, 3))
            record["exitcode"] = result.returncode
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            record["reason"] = str(error)
        finally:
            record["completed_after_seconds"] = time.monotonic() - self.started
            record["files"] = [self.file_receipt(path) for path in (report, stdout, stderr)]
        receipt = record["files"][0]
        if record.get("exitcode") == 0 and receipt.get("status") == "retained" \
                and receipt.get("bytes", 0) > 0:
            record["status"] = "captured"
        else:
            record.setdefault("reason", "Sampler did not produce a complete bounded report")
        return True
