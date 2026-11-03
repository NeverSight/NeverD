#!/usr/bin/env python3
"""Exercise real GUI argv handling without running any provider process."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(gui):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1")
    with tempfile.TemporaryDirectory(prefix="neverd-plugin-launch-") as name:
        directory = Path(name)
        marker = directory / "provider-ran"
        provider = directory / "provider.py"
        provider.write_text(
            "from pathlib import Path\n"
            f"Path({str(marker)!r}).write_text('unexpected startup execution')\n",
            encoding="utf-8",
        )
        manifest = directory / "plugin.json"
        manifest.write_text(json.dumps({
            "schema": "neverd.gui-plugin.v1", "id": "launch-test",
            "name": "Launch test", "version": "1.0.0",
            "program": sys.executable, "arguments": [str(provider)],
            "queries": [], "actions": [{"id": "inspect", "title": "Inspect",
                                         "context": "none", "context_menu": True}],
        }), encoding="utf-8")
        for spelling in ("--plugin", "--process-plugin", "--plugin="):
            args = ([spelling + str(manifest)] if spelling.endswith("=")
                    else [spelling, str(manifest)])
            result = subprocess.run([gui, *args, "--smoke-test"], env=env,
                                    capture_output=True, timeout=10)
            assert result.returncode == 0, (spelling, result.returncode, result.stderr)
            assert not marker.exists(), "Registration must not execute the provider"
            # An absent registration is observable through this negative control:
            # Qt used to consume --plugin, allowing the same invalid path to pass.
            absent = str(directory / "missing.json")
            args = ([spelling + absent] if spelling.endswith("=")
                    else [spelling, absent])
            result = subprocess.run([gui, *args, "--smoke-test"], env=env,
                                    capture_output=True, timeout=10)
            assert result.returncode == 2, (spelling, result.returncode, result.stderr)
            assert b"Cannot load plugin" in result.stderr, result.stderr
    print("GUI plugin CLI: canonical and compatibility spellings registered; "
          "invalid manifests refused; provider not executed at startup")


if __name__ == "__main__":
    run(sys.argv[1])
