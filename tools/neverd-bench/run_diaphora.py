"""NeverD-authored completion adapters for installed Diaphora APIs.

Run export inside IDAPython, or compare with the selected Python environment.
The selected Diaphora installation and its notices are archived by the runner.
"""
import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import traceback


HASH_BLOCK_BYTES = 1024 * 1024


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(HASH_BLOCK_BYTES), b""):
            value.update(block)
    return value.hexdigest()


def artifact(path, root):
    return {"path": os.path.relpath(path, root), "sha256": digest(path)}


def export(output, result, profile):
    import ida_auto
    import ida_kernwin
    import ida_nalt
    import diaphora_ida

    if not ida_auto.auto_wait():
        raise RuntimeError("IDA analysis was cancelled")
    identity = ida_nalt.retrieve_input_file_sha256()
    if identity is None:
        raise RuntimeError("IDA input SHA-256 is unavailable")
    result.update(input_sha256=identity.hex(), ida_version=ida_kernwin.get_kernel_version(),
                  database_path=str(output), profile=profile,
                  image_base=hex(ida_nalt.get_imagebase()))
    if profile["decompiler"]:
        import ida_hexrays

        if not ida_hexrays.init_hexrays_plugin():
            raise RuntimeError("The selected profile requires an available Hex-Rays decompiler")
    exporter = diaphora_ida.CIDABinDiff(str(output))
    exporter.use_decompiler = profile["decompiler"]
    exporter.export_microcode = profile["microcode"]
    exporter.exclude_library_thunk = not profile["include_library_thunks"]
    if exporter.export() is not True or ida_kernwin.user_cancelled():
        raise RuntimeError("Diaphora export did not complete")
    if Path(str(output) + "-crash").exists():
        raise RuntimeError("Diaphora retained an incomplete export marker")
    result["export_sha256"] = digest(output)


def compare(output, result):
    import diaphora

    before, after = [Path(os.environ["NEVERD_BENCH_DIAPHORA_" + side]) for side in ("PRIMARY", "SECONDARY")]
    exports = [Path(str(path) + ".json") for path in (before, after)]
    declarations = [json.loads(path.read_text()) for path in exports]
    result["input_sha256"] = [value["input_sha256"] for value in declarations]
    for side, path, receipt in zip(("primary", "secondary"), (before, after), exports):
        result[side] = {"database": artifact(path, output.parent),
                        "export_receipt": artifact(receipt, output.parent)}
    matcher = diaphora.CBinDiff(str(before))
    # This is also the standalone Diaphora CLI's name policy.
    matcher.ignore_all_names = False
    # Diaphora joins heuristic threads, but their exceptions do not make diff()
    # return False. Retain the failure before admitting a completed result.
    result["worker_errors"] = []
    previous_hook = threading.excepthook
    def worker_failed(args):
        result["worker_errors"].append({
            "thread": args.thread.name,
            "error": "".join(traceback.format_exception(args.exc_type, args.exc_value, args.exc_traceback)),
        })
        previous_hook(args)
    threading.excepthook = worker_failed
    try:
        if matcher.diff(str(after)) is not True:
            raise RuntimeError("Diaphora matching did not complete")
        if result["worker_errors"]:
            raise RuntimeError("Diaphora heuristic worker failed")
        matcher.save_results(str(output))
    finally:
        threading.excepthook = previous_hook
        matcher.db_close()
    for value, path in zip(declarations, (before, after)):
        if value["export_sha256"] != digest(path):
            raise RuntimeError("Diaphora modified an admitted export")
    result["result"] = artifact(output, output.parent)


mode = os.environ["NEVERD_BENCH_DIAPHORA_MODE"]
output = Path(os.environ["NEVERD_BENCH_DIAPHORA_OUTPUT"])
receipt = Path(str(output) + ".json")
result = {"schema": "neverd.benchmark.diaphora-" + mode + ".v1", "complete": False,
          "python_version": sys.version}
exit_code = 1
try:
    if mode not in ("export", "match") or output.exists() or receipt.exists():
        raise RuntimeError("Expected a known mode and new output paths")
    sys.path.insert(0, os.environ["NEVERD_BENCH_DIAPHORA_SOURCE"])
    import diaphora

    result["diaphora_version"] = diaphora.VERSION_VALUE
    if mode == "export":
        export(output, result, json.loads(os.environ["NEVERD_BENCH_DIAPHORA_PROFILE"]))
    else:
        compare(output, result)
    result["complete"] = True
    exit_code = 0
except BaseException:
    result["error"] = traceback.format_exc()
try:
    with receipt.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
except Exception:
    traceback.print_exc()
    exit_code = 1
if mode == "export":
    import ida_pro

    ida_pro.qexit(exit_code)
else:
    raise SystemExit(exit_code)
