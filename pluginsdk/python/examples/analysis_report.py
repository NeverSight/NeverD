from __future__ import annotations

import json

from neverd_plugin import Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="NeverD contributors",
    description="Prints a compact JSON inventory for the active binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_run(self, session: Session, arg: int) -> int:
        if not session.loaded:
            print(json.dumps({"error": "no binary loaded"}))
            return 1
        if arg:
            session.analyze()
        report = {
            "path": session.file_path,
            "architecture": session.architecture,
            "format": session.format,
            "bitness": session.bitness,
            "functions": session.function_count,
            "imports": session.import_count,
            "exports": session.export_count,
        }
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
