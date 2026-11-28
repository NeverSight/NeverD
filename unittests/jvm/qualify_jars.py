"""Build deterministic JARs from this directory's owned class fixtures."""

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

from qualify import SOURCES


def produce(jdk, output):
    rows = {
        r["id"]: base64.b64decode(r["bytes"])
        for r in json.loads((SOURCES / "classes.json").read_text())["fixtures"]
    }
    jars = []

    def record(name, data):
        jars.append(
            {
                "id": name,
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": base64.b64encode(data).decode(),
            }
        )

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        members = {
            "Sample.class": rows["java-8"],
            "META-INF/versions/11/Sample.class": rows["java-11"],
            "META-INF/versions/21/Sample.class": rows["java-21"],
            "Sample.java": (SOURCES / "Sample.java").read_bytes(),
        }
        for name, data in members.items():
            f = root / name
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(data)
        for mode in ["multi", "base"]:
            manifest = root / "main.mf"
            manifest.write_text(
                "Manifest-Version: 1.0\nMulti-Release: "
                + ("true" if mode == "multi" else "false")
                + "\n\n"
            )
            jar = root / "sample.jar"
            subprocess.run(
                [
                    jdk / "bin/jar",
                    "--create",
                    "--file",
                    jar,
                    "--date=2025-01-01T00:00:00Z",
                    "--manifest",
                    manifest,
                    *[arg for name in members for arg in ("-C", str(root), name)],
                ],
                check=True,
            )
            record(mode, jar.read_bytes())

        def zip_bytes(items):
            out = io.BytesIO()
            with zipfile.ZipFile(out, "w") as z:
                for name, data in items:
                    z.writestr(
                        zipfile.ZipInfo(name, (2025, 1, 1, 0, 0, 0)),
                        data,
                        compress_type=zipfile.ZIP_DEFLATED,
                    )
            return out.getvalue()

        record(
            "mixed",
            zip_bytes(
                [
                    ("Broken.class", b"\xca\xfe\xba\xbe\0"),
                    ("Sample.class", rows["java-17"]),
                    ("nested.jar", b"PK\x03\x04"),
                ]
            ),
        )
        record("classpath-alias", zip_bytes([("Other.class", rows["java-17"])]))
        record("traversal", zip_bytes([("../Sample.class", rows["java-8"])]))
        record(
            "duplicate",
            zip_bytes(
                [("Sample.class", rows["java-8"]), ("Sample.class", rows["java-11"])]
            ),
        )
        record(
            "manifest-sections",
            zip_bytes(
                [
                    (
                        "META-INF/MANIFEST.MF",
                        b"Manifest-Version: 1.0\r\n\r\nName: Sample.class\r\nMulti-Release: true\r\n\r\n",
                    ),
                    *members.items(),
                ]
            ),
        )
        record(
            "manifest-fold",
            zip_bytes(
                [
                    (
                        "META-INF/MANIFEST.MF",
                        b"Manifest-Version: 1.0\r\nmUlTi-ReLeAsE: tr\r\n ue\r\n\r\n",
                    ),
                    *members.items(),
                ]
            ),
        )
    output.write_text(json.dumps({"fixtures": jars}, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=SOURCES / "jars.json")
    args = parser.parse_args()
    produce(args.jdk, args.output)
