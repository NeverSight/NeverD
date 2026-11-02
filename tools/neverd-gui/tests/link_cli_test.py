#!/usr/bin/env python3
"""The link client must work without loading a graphical platform plugin."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def desktop_handler(executable, directory, environment):
    if not sys.platform.startswith("linux"):
        return
    validator, launcher = shutil.which("desktop-file-validate"), shutil.which("gio")
    if not validator or not launcher:
        print("Desktop launch check skipped: desktop-file-validate or gio unavailable")
        return
    # Neither xdg-mime nor the real user's associations are touched. Exercise
    # the generated Exec with the desktop implementation's actual parser.
    directory = Path(directory)
    helpers = directory / "helpers"
    helpers.mkdir()
    helper = helpers / "xdg-mime"
    helper.write_text("#!/bin/sh\nexit 0\n")
    helper.chmod(0o755)
    environment = dict(environment, XDG_DATA_HOME=str(directory / "data"),
                       XDG_CONFIG_HOME=str(directory / "config"),
                       PATH=str(helpers) + os.pathsep + environment["PATH"])
    installed = directory / 'installed $money `literal` \\ "quote" 50%'
    installed.mkdir()
    binary = installed / "neverd-gui"
    shutil.copy2(executable, binary)
    subprocess.run([str(binary), "--smoke-test", "--register-link-handler"],
                   env=environment, check=True, capture_output=True, timeout=20)
    entry = directory / "data/applications/neverd-links.desktop"
    subprocess.run([validator, str(entry)], check=True, capture_output=True)
    # Replace only the private copied executable with an argument recorder.
    output = directory / "launched.json"
    temporary = str(output) + ".tmp"
    binary.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                      f"with open({temporary!r}, 'w') as record:\n"
                      "    json.dump(sys.argv, record)\n"
                      f"os.replace({temporary!r}, {str(output)!r})\n")
    binary.chmod(0o755)
    uri = "neverd:///file%20name.bin/addresses?ea=0x123&view=disasm"
    subprocess.run([launcher, "launch", str(entry), uri], env=environment,
                   check=True, capture_output=True, timeout=10)
    deadline = time.monotonic() + 10
    while not output.exists() and time.monotonic() < deadline:
        time.sleep(.01)
    assert json.loads(output.read_text()) == [str(binary), "--link", uri]
    # A hung desktop helper must return a visible failure within its budget.
    helper.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(60)\n")
    result = subprocess.run([executable, "--smoke-test", "--register-link-handler"],
                            env=environment, capture_output=True, text=True,
                            timeout=20)
    assert result.returncode != 0 and "xdg-mime" in result.stderr, result
    result = subprocess.run([executable, "--smoke-test", "--unregister-link-handler"],
                            env=environment, capture_output=True, timeout=10)
    assert result.returncode == 0 and not entry.exists(), result


def run(executable):
    environment = dict(os.environ)
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "QT_QPA_PLATFORM", "GDK_BACKEND"):
        environment.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="neverd-link-client-") as directory:
        name = Path(directory).name
        uri = f"neverd:///{name}.bin/addresses?ea=0x1"
        cases = [
            (["--list-instances"], 0, None),
            (["--no-launch", "--link", "https://example.com"], 2,
             "A link starts with neverd:// or ida://."),
            (["--no-launch", "--link", ""], 2,
             "A link starts with neverd:// or ida://."),
            (["--no-launch", uri], 1,
             "No running NeverD has the link's program open."),
            (["--version"], 0, "NeverD"),
            (["--help"], 0, "--list-instances"),
        ]
        for arguments, expected, message in cases:
            # Automated settings use a private IniFormat directory on Windows
            # as well; this never changes the user's registry preferences.
            result = subprocess.run([executable, "--smoke-test", *arguments],
                                    env=environment, capture_output=True,
                                    text=True, timeout=30)
            assert result.returncode == expected, (arguments, result)
            assert result.stdout or result.stderr, (arguments, result)
            if message:
                assert message in result.stdout + result.stderr, (arguments, result)
        desktop_handler(executable, directory, environment)
    print("Headless instance listing, refusal paths, bare links, help and version passed")


if __name__ == "__main__":
    run(sys.argv[1])
