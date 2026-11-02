"""Export one freshly analyzed input using an explicitly selected BinExport."""
import hashlib
import json
import os
from pathlib import Path
import traceback

import ida_auto
import ida_expr
import ida_idaapi
import ida_kernwin
import ida_loader
import ida_nalt
import ida_pro
import idautils


output = Path(os.environ["NEVERD_BENCH_BINEXPORT_OUTPUT"])
receipt = Path(os.environ["NEVERD_BENCH_BINEXPORT_RECEIPT"])
result = {"schema": "neverd.benchmark.binexport.v1", "complete": False}
exit_code = 1
try:
    if output.exists() or receipt.exists():
        raise RuntimeError("export and receipt paths must be new")
    if not ida_auto.auto_wait():
        raise RuntimeError("IDA analysis was cancelled")
    identity = ida_nalt.retrieve_input_file_sha256()
    if identity is None:
        raise RuntimeError("IDA did not retain the input SHA-256")
    result["input_sha256"] = identity.hex()
    result["ida_version"] = ida_kernwin.get_kernel_version()
    result["image_base"] = hex(ida_nalt.get_imagebase())
    result["function_entries"] = [hex(entry) for entry in idautils.Functions()]
    plugin = ida_loader.load_plugin(os.environ["NEVERD_BENCH_BINEXPORT_PLUGIN"])
    if plugin is None:
        raise RuntimeError("the selected BinExport plugin did not load")
    value = ida_expr.idc_value_t()
    error = ida_expr.eval_idc_expr(
        value, ida_idaapi.BADADDR, "BinExportBinary(" + json.dumps(str(output)) + ")")
    if error:
        raise RuntimeError(error)
    if not output.is_file() or not output.stat().st_size:
        raise RuntimeError("BinExport did not produce a nonempty output")
    result["export_sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
    result["complete"] = True
    exit_code = 0
except Exception:
    result["error"] = traceback.format_exc()
try:
    with receipt.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
except Exception:
    traceback.print_exc()
    exit_code = 1
ida_pro.qexit(exit_code)
