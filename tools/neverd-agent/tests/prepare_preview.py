"""Prepare isolated, explicitly illustrative Agent projects for native captures.

This only creates new directories under OUTPUT. It does not execute or modify
the supplied binaries, write the user's Agent settings, or claim analysis ran.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--binary", action="append", type=Path, required=True)
    parser.add_argument("--gui", default="")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    projects = []
    stamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
    for index, name in enumerate(["Windows API 研究", "调用关系笔记"]):
        root = output / name
        project_id = str(uuid4())
        objects = []
        for binary in args.binary:
            binary = binary.resolve(strict=True)
            with binary.open("rb") as source:
                hasher = hashlib.sha256()
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    hasher.update(block)
                digest = hasher.hexdigest()
            object_id = str(uuid4())
            objects.append({"id": object_id, "name": binary.name,
                            "path": str(binary), "sha256": digest})
            records = []
            if index == 1:
                records.append({"id": str(uuid4()), "originProjectId": project_id,
                                "originObjectId": object_id, "createdAt": stamp,
                                "text": "界面检查用的手动记录：后续需要核对导出入口与参数关系。此条不是分析结论。"})
            write_json(root / ".nd-agent" / "objects" / object_id / "memory.json",
                       {"version": 1, "kind": "neverd-object-memory", "sha256": digest,
                        "objectName": binary.name, "records": records})
        write_json(root / ".nd-agent" / "project.json",
                   {"version": 1, "id": project_id, "name": name, "objects": objects})
        write_json(root / ".nd-agent" / "router" / "conversation.json",
                   {"version": 1, "draft": "从导出入口开始梳理，并记录每一步的证据。",
                    "messages": [{"id": str(uuid4()), "role": "user", "objectId": "",
                                  "createdAt": stamp, "status": "saved",
                                  "text": "请比较这几个分析对象中的文件打开流程，整理参数如何在调用之间传递。"}]})
        (root / "notes").mkdir()
        (root / "notes" / "research.md").write_text(
            "# 研究笔记\n\n这是原生界面检查工作区，尚未执行模型或二进制分析。\n", encoding="utf-8")
        projects.append((project_id, root))
    write_json(output / "global" / "config.json",
               {"version": 1, "projects": [str(root) for _, root in projects],
                "activeProjectId": projects[0][0], "sharing": True,
                "lightTheme": False, "guiPath": args.gui})
    print(output / "global")


if __name__ == "__main__":
    main()
