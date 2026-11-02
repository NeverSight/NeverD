"""Replay a NeverD interaction report in a disposable IDA GUI.

Run with IDA's -S option: ida_link_paint.py NEVERD_REPORT OUTPUT.
Requires IDAPython with PySide6. Measures the GUI navigation API through
the event-loop turn after its disassembly viewport paints the target.
It exits this IDA process when finished; never run in a user's database.
"""
import hashlib
import json
from pathlib import Path
import time

import ida_auto
import ida_kernwin
import ida_nalt
import ida_pro
import idaapi
import idc
from PySide6.QtCore import QEvent, QObject, QTimer, qVersion
from PySide6.QtWidgets import QAbstractScrollArea, QApplication


class LinkPaintProbe(QObject):
    def __init__(self, manifest, output):
        super().__init__(QApplication.instance())
        self.output = output
        self.timeout_ms = manifest["sample_timeout_ms"]
        self.phases = {}
        self.jobs = [(name, row) for name in
                     ("jump_cold", "jump_warm", "link_addresses", "link_functions")
                     for row in manifest["phases"][name]["detail"]]
        self.index = 0
        self.pending = False
        self.paint_queued = False
        self.limit = QTimer(self)
        self.limit.setSingleShot(True)
        self.limit.timeout.connect(lambda: self.finish(False, "sample timeout"))
        widget = ida_kernwin.find_widget("IDA View-A")
        if not widget:
            raise RuntimeError("IDA View-A is unavailable")
        ida_kernwin.activate_widget(widget, True)
        ida_kernwin.set_view_renderer_type(widget, ida_kernwin.TCCRT_FLAT)
        # The legacy PySide helper expects QtGui.QWidget.FromCapsule. IDA's
        # Qt 6 bridge wraps the actual widget type through shiboken instead.
        self.view = ida_kernwin.PluginForm.TWidgetToQtPythonWidget(widget)
        if not isinstance(self.view, QAbstractScrollArea):
            raise RuntimeError("IDA's disassembly has no measurable Qt viewport")
        self.viewport = self.view.viewport()
        self.viewport.installEventFilter(self)
        self.report = {
            "schema_version": 1, "success": False,
            "ida_version": idaapi.get_kernel_version(), "qt_version": qVersion(),
            "input_sha256": hashlib.sha256(
                Path(ida_nalt.get_input_file_path()).read_bytes()).hexdigest(),
            "viewport": [self.viewport.width(), self.viewport.height()],
            "measurement": "Navigation API through the event-loop turn after "
                           "the disassembly viewport paints; not hardware presentation.",
            "phases": self.phases,
        }
        QTimer.singleShot(0, self.next)

    def next(self):
        if self.index == len(self.jobs):
            self.finish(True)
            return
        name, row = self.jobs[self.index]
        self.expected = int(row["address"], 0)
        self.pending = True
        self.paint_queued = False
        self.started = time.perf_counter_ns()
        self.limit.start(self.timeout_ms)
        if "uri" in row:
            accepted = ida_kernwin.open_ida_link(row["uri"])
        else:
            accepted = ida_kernwin.jumpto(self.expected)
        if not accepted:
            self.finish(False, f"navigation refused: {name} {row}")
            return
        # Match NeverD's afterPaint: request a frame even for an already
        # displayed target, and observe the viewport rather than its chrome.
        self.viewport.update()

    def eventFilter(self, obj, event):
        if (obj == self.viewport and event.type() == QEvent.Type.Paint
                and self.pending and not self.paint_queued):
            self.paint_queued = True
            QTimer.singleShot(0, self.painted)
        return False

    def painted(self):
        elapsed = (time.perf_counter_ns() - self.started) / 1e6
        if ida_kernwin.get_screen_ea() != self.expected:
            self.finish(False, "painted viewport did not reach the target")
            return
        self.pending = False
        self.limit.stop()
        name, row = self.jobs[self.index]
        self.phases.setdefault(name, []).append({
            "address": row["address"], "uri": row.get("uri"), "ms": elapsed})
        self.index += 1
        QTimer.singleShot(0, self.next)

    def finish(self, success, reason=""):
        self.pending = False
        self.limit.stop()
        self.report.update(success=success, failure_reason=reason)
        self.output.write_text(json.dumps(self.report, indent=2) + "\n")
        ida_pro.qexit(0 if success else 1)


ida_auto.auto_wait()
_manifest = json.loads(Path(idc.ARGV[1]).read_text())
_output = Path(idc.ARGV[2])


def start():
    global _probe
    try:
        _probe = LinkPaintProbe(_manifest, _output)
    except Exception as error:
        _output.write_text(json.dumps({"success": False, "failure_reason": str(error)}))
        ida_pro.qexit(1)


QTimer.singleShot(500, start)
