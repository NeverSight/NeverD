"""Run through `neverd plugins`; Session is supplied by the real Python host."""

import json
from pathlib import Path

from neverd_plugin import Plugin, Session


@Plugin(name="Source Patch Example", version="1.0.0", author="NeverD contributors")
class SourcePatchExample:
    def on_run(self, session: Session, arg: int) -> int:
        config = json.loads(
            Path(session.file_path).with_name("example.json").read_text(encoding="utf-8")
        )
        report = session.patch_source(
            Path(config["source"]).read_bytes(),
            config["sdk_output"],
            mappings={"replacement": int(config["address"], 0)},
            compiler="clang",
            compiler_path=config["clang"],
            artifact="object",
            bindings={"saved_target": ("original", int(config["address"], 0))},
            dependencies={
                "extra_function": (config["dependency"], "function"),
                "extra_state": (config["dependency"], "data"),
            },
        )
        Path(config["sdk_report"]).write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
        return 0
