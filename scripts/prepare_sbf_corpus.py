"""Verify cached SBF assets; download only with explicit --fetch authorization."""

import argparse
from pathlib import Path
import tempfile
import urllib.request

try:
    from .sbf_corpus_common import DEFAULT_MANIFEST, LIMITS, load_manifest, verify_artifact
except ImportError:
    from sbf_corpus_common import DEFAULT_MANIFEST, LIMITS, load_manifest, verify_artifact


def prepare(manifest_path, directory, fetch=False):
    manifest = load_manifest(manifest_path)
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for row in manifest["artifacts"]:
        target = directory / row["file"]
        if target.exists() or target.is_symlink():
            verify_artifact(target, row)
            continue
        if not fetch:
            raise FileNotFoundError(f"{target}: missing; use --fetch to acquire pinned asset")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=directory, delete=False) as stream:
                temporary = Path(stream.name)
                request = urllib.request.Request(row["url"], headers={"User-Agent": "NeverD-corpus"})
                with urllib.request.urlopen(request, timeout=LIMITS.download_timeout_seconds) as response:
                    remaining = row["size"]
                    while block := response.read(min(1024 * 1024, remaining + 1)):
                        remaining -= len(block)
                        if remaining < 0:
                            raise ValueError("download exceeds declared size")
                        stream.write(block)
            verify_artifact(temporary, row)
            temporary.replace(target)
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--fetch", action="store_true")
    args = parser.parse_args()
    value = prepare(args.manifest, args.cache, args.fetch)
    print(f"Verified {len(value['artifacts'])} pinned SBF assets")


if __name__ == "__main__":
    main()
