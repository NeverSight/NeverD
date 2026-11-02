#!/usr/bin/env python3
"""Wait for the test window manager to own the X11 desktop before mapping windows."""

import argparse
import os
import re
import subprocess
import time


STARTUP_TIMEOUT_SECONDS = 10
POLL_INTERVAL_SECONDS = 0.05


def owner(window):
    result = subprocess.run(
        ["xprop", *window, "_NET_SUPPORTING_WM_CHECK"],
        capture_output=True,
        text=True,
        timeout=1,
        check=False,
    )
    match = re.search(r"window id # (0x[0-9a-fA-F]+)", result.stdout)
    return int(match[1], 16) if result.returncode == 0 and match else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int, help="PID of the test window manager")
    args = parser.parse_args()
    if args.pid <= 0:
        parser.error("the window manager PID must be positive")
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            os.kill(args.pid, 0)
            window = owner(["-root"])
            # The supporting window must identify itself too; a stale root
            # property from an exited manager is not a ready desktop.
            if window and owner(["-id", hex(window)]) == window:
                return
        except ProcessLookupError:
            raise SystemExit("The test window manager exited before becoming ready")
        except subprocess.TimeoutExpired:
            pass
        time.sleep(POLL_INTERVAL_SECONDS)
    raise SystemExit("Timed out waiting for the test window manager to own X11")


if __name__ == "__main__":
    main()
