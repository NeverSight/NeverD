#!/usr/bin/env python3
"""Linux worker THP overrides must stay out of SDK hosts."""
import ctypes
import os
from pathlib import Path
import sys
import tempfile

from idle_memory_test import source
from transport_test import Client


def thp_enabled(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("THP_enabled:"):
            return int(line.split()[1])
    return None


def run(executable, library):
    inherited = thp_enabled(os.getpid())
    if inherited is None:
        print("THP policy: /proc does not expose the process THP state")
        return 77
    # A host loading the SDK keeps its own process-memory policy.
    sdk = ctypes.CDLL(library)
    assert sdk is not None
    assert thp_enabled(os.getpid()) == inherited, "SDK changed host THP policy"
    option = "MIMALLOC_ALLOW_THP"
    previous = os.environ.get(option)
    expected_source = None
    try:
        with tempfile.TemporaryDirectory(prefix="neverd-thp-policy-") as directory:
            binary = Path(directory) / "arithmetic.evm"
            binary.write_text("0x600160020100", encoding="ascii")
            for value, expected in (("2", inherited), ("1", inherited), ("0", 0)):
                os.environ[option] = value
                client = Client(executable)
                try:
                    assert thp_enabled(client.process.pid) == expected, (value, expected)
                    opened = client.call("open", {
                        "path": str(binary), "read_only": True,
                        "analysis": False, "warm": False,
                    })
                    assert opened["status"] == "ok", opened
                    assert client.call("functions")["status"] == "ok"
                    actual = source(client)
                    assert actual
                    if expected_source is None:
                        expected_source = actual
                    assert actual == expected_source, value
                finally:
                    client.close()
    finally:
        if previous is None:
            os.environ.pop(option, None)
        else:
            os.environ[option] = previous
    assert thp_enabled(os.getpid()) == inherited, "worker changed parent THP policy"
    print(f"THP policy: parent={inherited}; overrides, full C and SDK host preserved")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1], sys.argv[2]))
