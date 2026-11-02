"""Shared admission, resource policy and atomic reports for opt-in SBF corpora."""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import tempfile


@dataclass(frozen=True)
class CorpusLimits:
    max_artifacts: int = 4096
    max_file_bytes: int = 64 * 1024 * 1024
    download_timeout_seconds: int = 60
    stage_timeout_seconds: int = 60
    worker_address_space_bytes: int = 8 * 1024 * 1024 * 1024
    max_output_bytes: int = 512 * 1024 * 1024
    max_report_bytes: int = 8 * 1024 * 1024
    default_jobs: int = 2


LIMITS = CorpusLimits()
DEFAULT_MANIFEST = Path(__file__).resolve().parents[1] / (
    "unittests/sbf/corpus/solana-dex-2026-10-24.json"
)


def analysis_stages():
    authority = Path(__file__).resolve().parents[1] / "include/neverd/sbf/analysis/SBFAnalysisStages.def"
    stages = re.findall(r'^SBF_ANALYSIS_STAGE\(\w+,\s*"([^"]+)"\)', authority.read_text(), re.M)
    if not stages or len(stages) != len(set(stages)):
        raise ValueError("invalid SBF stage definition authority")
    return tuple(stages)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=path.parent, delete=False,
                                     encoding="utf-8") as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, indent=2)
        stream.write("\n")
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path):
    path = Path(path)
    if path.stat().st_size > LIMITS.max_report_bytes:
        raise ValueError("manifest exceeds byte budget")
    value = json.loads(path.read_text())
    rows = value.get("artifacts")
    if type(value.get("schema")) is not int or value["schema"] != 1 or not isinstance(rows, list) or not rows:
        raise ValueError("expected nonempty schema-1 artifact manifest")
    if len(rows) > LIMITS.max_artifacts:
        raise ValueError("manifest exceeds artifact budget")
    profile = value["profile"]
    if profile.get("purpose", "execution") != "execution":
        raise ValueError("corpus qualification requires an execution profile")
    if profile["cluster"] not in ("mainnet-beta", "testnet", "devnet", "localnet"):
        raise ValueError("unknown cluster profile")
    if profile["slot"] is not None and (type(profile["slot"]) is not int or profile["slot"] < 0):
        raise ValueError("invalid execution profile slot")
    names, hashes, slugs = set(), set(), set()
    for row in rows:
        name, digest = row["file"], row["sha256"]
        slug = row["name"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", slug) or slug in (".", "..") or slug in slugs:
            raise ValueError("unsafe or duplicate artifact name")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\.so", name) or name in names:
            raise ValueError("unsafe or duplicate artifact filename")
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or digest in hashes:
            raise ValueError("invalid or duplicate artifact SHA-256")
        for key in ("size", "onchain_size"):
            if type(row[key]) is not int or not 0 < row[key] <= LIMITS.max_file_bytes:
                raise ValueError("artifact size exceeds byte budget")
        if row["onchain_size"] < row["size"]:
            raise ValueError("onchain size is smaller than stored size")
        if not re.fullmatch(r"[0-9a-f]{64}", row["onchain_sha256"]):
            raise ValueError("invalid onchain SHA-256")
        expected_url = ("https://github.com/" + value["repository"] +
                        "/releases/download/" + value["release_tag"] + "/" + name)
        if row["url"] != expected_url:
            raise ValueError("asset URL does not match pinned snapshot")
        if row["loader"] not in ("loader-v1", "loader-v2", "loader-v3"):
            raise ValueError("unknown loader ownership")
        names.add(name)
        hashes.add(digest)
        slugs.add(slug)
    return value


def verify_artifact(path, row):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("artifact is missing or is a symlink")
    if path.stat().st_size != row["size"] or sha256(path) != row["sha256"]:
        raise ValueError("artifact size/SHA-256 mismatch")
    raw = hashlib.sha256(path.read_bytes())
    raw.update(bytes(row["onchain_size"] - row["size"]))
    if raw.hexdigest() != row["onchain_sha256"]:
        raise ValueError("padded onchain SHA-256 mismatch")
