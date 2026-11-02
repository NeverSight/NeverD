#!/usr/bin/env python3
"""Compare painted deep-link navigation in the production GUI and IDA.

Linux diagnostic on one private Xvfb display, with fresh disposable input
copies and application settings. Warm OS caches; no hardware presentation
or whole-program analysis/decompilation performance claim.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import signal
import statistics
import subprocess
import tempfile
import time


def identity(path):
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "bytes": path.stat().st_size}


def distribution(values):
    ordered = sorted(values)
    return {"samples": len(values), "median_ms": statistics.median(values),
            "p95_ms": ordered[math.ceil(len(values) * .95) - 1],
            "p99_ms": ordered[math.ceil(len(values) * .99) - 1], "raw_ms": values}


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def execute(command, environment, log, timeout):
    with log.open("w") as output:
        process = subprocess.Popen(command, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
            if code:
                raise RuntimeError(f"exit {code}: see {log}")
        finally:
            stop(process)


def display():
    for number in range(130, 180):
        if (Path(f"/tmp/.X11-unix/X{number}").exists()
                or Path(f"/tmp/.X{number}-lock").exists()):
            continue
        process = subprocess.Popen(
            ["Xvfb", f":{number}", "-screen", "0", "1600x1000x24", "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.monotonic() + 10
        while process.poll() is None and time.monotonic() < deadline:
            if Path(f"/tmp/.X11-unix/X{number}").exists():
                return process, number
            time.sleep(.05)
        stop(process)
        raise RuntimeError(f"could not start Xvfb :{number}")
    raise RuntimeError("no unused private X display")


def checked_report(path):
    report = json.loads(path.read_text())
    if not report.get("success"):
        raise RuntimeError(f"{path}: {report.get('failure_reason')}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("gui", "worker", "engine", "ida", "input", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--samples", type=int, default=3, help="fresh process pairs")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("this diagnostic requires Linux, Xvfb and Qt's xcb backend")
    if args.samples < 1 or not math.isfinite(args.timeout) or args.timeout < 1:
        parser.error("samples and timeout must be positive")
    for name in ("gui", "worker", "engine", "ida", "input", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    probe_source = Path(__file__).with_name("ida_link_paint.py")
    report = {"schema_version": 1, "kind": "gui-deep-link-painted",
              "source_revision": args.source_revision, "host": platform.platform(),
              "load_average_before": os.getloadavg(), "cpu_count": os.cpu_count(),
              "artifacts": {name: identity(getattr(args, name))
                            for name in ("gui", "worker", "engine", "ida", "input")},
              "ida_probe": identity(probe_source), "runs": [], "success": False,
              "conditions": [
                  "Private Xvfb display 1600x1000x24, xcb; fresh process pairs and settings.",
                  "Same input and URI sequence; preliminary jumps warm both listings.",
                  "Time from navigation API (NeverD includes URL parsing) to the event-loop "
                  "turn after the disassembly viewport paints; target cursor is validated.",
                  "Both request a viewport update even if the target is already displayed.",
                  "IDA uses an IDAPython/PySide6 observer; NeverD's observer is native C++.",
                  "No CLI startup or forwarding; no hardware presentation timing.",
                  "Warm OS caches; host activity uncontrolled; builds and tests should be idle."]}
    server, number = display()
    try:
        with tempfile.TemporaryDirectory(prefix="neverd-gui-links-") as directory:
            root = Path(directory)
            root.chmod(0o700)
            for index in range(args.samples):
                scratch = root / str(index)
                scratch.mkdir(mode=0o700)
                binary = scratch / args.input.name
                shutil.copyfile(args.input, binary)
                runtime = scratch / "runtime"
                runtime.mkdir(mode=0o700)
                environment = {key: value for key, value in os.environ.items()
                               if key not in ("WAYLAND_DISPLAY", "XAUTHORITY")}
                environment.update(DISPLAY=f":{number}", QT_QPA_PLATFORM="xcb",
                                   GDK_BACKEND="x11", XDG_SESSION_TYPE="x11",
                                   XDG_RUNTIME_DIR=str(runtime),
                                   XDG_CONFIG_HOME=str(scratch / "config"),
                                   XDG_DATA_HOME=str(scratch / "data"))
                stem = args.output.with_suffix("")
                neverd_path = Path(f"{stem}-{index}-neverd.json")
                ida_path = Path(f"{stem}-{index}-ida.json")
                # Old successful output must never mask a failed current launch.
                neverd_path.unlink(missing_ok=True)
                ida_path.unlink(missing_ok=True)
                execute([str(args.gui), "--worker", str(args.worker),
                         "--interaction-benchmark", str(neverd_path),
                         str(binary)],
                        environment, neverd_path.with_suffix(".log"), args.timeout + 10)
                neverd = checked_report(neverd_path)
                if neverd.get("schema_version") != 2:
                    raise RuntimeError("GUI report needs schema 2 painted-link phases")
                if neverd["input_sha256"] != report["artifacts"]["input"]["sha256"]:
                    raise RuntimeError("NeverD analyzed another input")
                for name in ("link_addresses", "link_functions"):
                    phase = neverd["phases"][name]
                    if phase["missed"] or not phase["samples"]:
                        raise RuntimeError(f"NeverD did not paint every {name} target")
                    if not phase["analysis_complete"]:
                        raise RuntimeError("NeverD's background analysis was still active")
                profile = scratch / "ida-profile"
                profile.mkdir(mode=0o700)
                for name in ("ida.reg", "idapro.hexlic", "ida-config.json", "cfg"):
                    source = Path.home() / ".idapro" / name
                    destination = profile / name
                    if source.is_dir():
                        shutil.copytree(source, destination)
                    elif source.is_file():
                        shutil.copy2(source, destination)
                        destination.chmod(0o600)
                script = scratch / "probe.py"
                shutil.copyfile(probe_source, script)
                environment["IDAUSR"] = str(profile)
                script_arguments = " ".join('"' + str(path).replace('"', '\\"') + '"'
                                            for path in (script, neverd_path, ida_path))
                execute([str(args.ida), "-A", "-c", f"-o{scratch / 'analysis.i64'}",
                         "-S" + script_arguments, str(binary)], environment,
                        ida_path.with_suffix(".log"), args.timeout + 10)
                ida = checked_report(ida_path)
                if ida["input_sha256"] != report["artifacts"]["input"]["sha256"]:
                    raise RuntimeError("IDA analyzed another input")
                for name in ("link_addresses", "link_functions"):
                    first = neverd["phases"][name]["detail"]
                    second = ida["phases"][name]
                    if [(row["address"], row["uri"]) for row in first] != [
                            (row["address"], row["uri"]) for row in second]:
                        raise RuntimeError("navigation sequences differ")
                report["runs"].append({"neverd": neverd, "ida": ida})
                print(f"painted pair {index + 1}/{args.samples}", flush=True)
            for name, artifact in report["artifacts"].items():
                if identity(getattr(args, name)) != artifact:
                    raise RuntimeError(f"{name} changed during the measurement")
            report["cases"] = {}
            for name in ("link_addresses", "link_functions"):
                cases = {}
                for tool in ("neverd", "ida"):
                    rows = [row for run in report["runs"] for row in
                            (run[tool]["phases"][name]["detail"] if tool == "neverd"
                             else run[tool]["phases"][name])]
                    cases[tool] = distribution([row["ms"] for row in rows])
                report["cases"][name] = cases
                print(name, {tool: {key: value for key, value in case.items()
                                    if key != "raw_ms"} for tool, case in cases.items()}, flush=True)
            report["success"] = True
    except Exception as error:
        report["failure_reason"] = str(error)
        raise
    finally:
        stop(server)
        report["load_average_after"] = os.getloadavg()
        args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
