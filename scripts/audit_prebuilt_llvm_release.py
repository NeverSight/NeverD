#!/usr/bin/env python3
"""Audit published prebuilt LLVM assets against NeverD's source pins."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlparse
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONSUMER = ROOT / "cmake" / "NeverDLLVMPrebuilt.cmake"
GITLINK_PATH = "third_party/llvm-project"
API_RESPONSE_LIMIT = 2 * 1024 * 1024
CHECKSUM_RESPONSE_LIMIT = 4096
ASSET_VARIABLES = (
    ("neverd-llvm-linux-x86_64.tar.xz", "_NEVERD_LLVM_PIN_LINUX_X86_64"),
    ("neverd-llvm-macos-arm64.tar.xz", "_NEVERD_LLVM_PIN_MACOS_ARM64"),
    ("neverd-llvm-windows-x64.zip", "_NEVERD_LLVM_PIN_WINDOWS_X64"),
)


class AuditError(ValueError):
    """Raised when source pins and the published release disagree."""


class FetchError(RuntimeError):
    """Raised when authoritative release metadata cannot be fetched."""


@dataclass(frozen=True)
class AssetPin:
    name: str
    digest: str


@dataclass(frozen=True)
class PinSet:
    repository: str
    tag: str
    commit: str
    assets: tuple[AssetPin, ...]


def _cmake_setting(source: str, name: str) -> str:
    pattern = re.compile(
        rf'^set\({re.escape(name)}[ \t\r\n]+"([^"]*)"', re.MULTILINE
    )
    matches = pattern.findall(source)
    if len(matches) != 1:
        raise AuditError(
            f"expected one CMake setting for {name}, found {len(matches)}"
        )
    return matches[0]


def parse_pins(source: str) -> PinSet:
    repository = _cmake_setting(source, "NEVERD_LLVM_PREBUILT_REPO")
    default_tag = _cmake_setting(source, "NEVERD_LLVM_PREBUILT_TAG")
    pinned_tag = _cmake_setting(source, "NEVERD_LLVM_PREBUILT_PINNED_TAG")
    commit = _cmake_setting(source, "NEVERD_LLVM_PREBUILT_PINNED_COMMIT")

    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise AuditError(f"invalid prebuilt LLVM repository: {repository!r}")
    if default_tag != pinned_tag:
        raise AuditError(
            "default prebuilt LLVM tag does not match the tag described by the pins"
        )
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise AuditError("pinned LLVM commit must be 40 lowercase hexadecimal digits")

    assets = []
    for asset_name, variable in ASSET_VARIABLES:
        digest = _cmake_setting(source, variable)
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise AuditError(f"{variable} must be a lowercase SHA-256 digest")
        assets.append(AssetPin(asset_name, digest))
    return PinSet(repository, pinned_tag, commit, tuple(assets))


def audit_gitlink(pins: PinSet, tree_entry: str) -> str:
    lines = tree_entry.splitlines()
    if len(lines) != 1:
        raise AuditError(
            f"expected one LLVM gitlink for {GITLINK_PATH}, found {len(lines)}"
        )
    match = re.fullmatch(
        rf"160000 commit ([0-9a-f]{{40}})\t{re.escape(GITLINK_PATH)}",
        lines[0],
    )
    if match is None:
        raise AuditError(f"invalid LLVM gitlink entry: {lines[0]!r}")

    gitlink = match.group(1)
    if gitlink != pins.commit:
        raise AuditError(
            "pinned LLVM commit does not match the submodule gitlink: "
            f"expected {gitlink}, got {pins.commit}"
        )
    return gitlink


def read_gitlink(repository_root: Path) -> str:
    try:
        completed = subprocess.run(
            ["git", "ls-tree", "HEAD", "--", GITLINK_PATH],
            cwd=repository_root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise FetchError(f"failed to read the LLVM gitlink: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or f"Git exited {completed.returncode}"
        raise FetchError(f"failed to read the LLVM gitlink: {detail}")
    return completed.stdout


def _release_assets(release: object) -> dict[str, Mapping[str, object]]:
    if not isinstance(release, Mapping):
        raise AuditError("release metadata is not an object")
    raw_assets = release.get("assets")
    if not isinstance(raw_assets, list):
        raise AuditError("release metadata has no assets list")

    assets = {}
    for index, raw_asset in enumerate(raw_assets):
        if not isinstance(raw_asset, Mapping):
            raise AuditError(f"release asset {index} is not an object")
        name = raw_asset.get("name")
        if not isinstance(name, str) or not name:
            raise AuditError(f"release asset {index} has no name")
        if name in assets:
            raise AuditError(f"duplicate release asset: {name}")
        assets[name] = raw_asset
    return assets


def audit_release(
    pins: PinSet,
    release: object,
    tag_ref: object,
    sidecars: Mapping[str, str],
) -> tuple[str, ...]:
    if not isinstance(release, Mapping):
        raise AuditError("release metadata is not an object")
    if release.get("tag_name") != pins.tag:
        raise AuditError(
            f"release tag {release.get('tag_name')!r} does not match {pins.tag!r}"
        )
    if release.get("target_commitish") != pins.commit:
        raise AuditError(
            "release target commit does not match the pinned LLVM gitlink: "
            f"expected {pins.commit}, got {release.get('target_commitish')!r}"
        )

    if not isinstance(tag_ref, Mapping):
        raise AuditError("Git tag reference metadata is not an object")
    tag_target = tag_ref.get("object")
    if not isinstance(tag_target, Mapping):
        raise AuditError("Git tag reference has no target object")
    if tag_target.get("type") != "commit" or tag_target.get("sha") != pins.commit:
        raise AuditError(
            "Git tag target does not match the pinned LLVM gitlink: "
            f"expected commit {pins.commit}, got {tag_target!r}"
        )

    release_assets = _release_assets(release)
    audited = []
    for asset in pins.assets:
        sidecar_name = f"{asset.name}.sha256"
        missing = [
            name for name in (asset.name, sidecar_name) if name not in release_assets
        ]
        if missing:
            raise AuditError("missing release asset: " + ", ".join(missing))

        published_digest = release_assets[asset.name].get("digest")
        expected_digest = f"sha256:{asset.digest}"
        if published_digest != expected_digest:
            raise AuditError(
                f"archive digest drift for {asset.name}: expected "
                f"{expected_digest}, got {published_digest!r}"
            )

        sidecar = sidecars.get(sidecar_name)
        expected_fields = [asset.digest, asset.name]
        if sidecar is None or sidecar.split() != expected_fields:
            raise AuditError(
                f"checksum sidecar drift for {sidecar_name}: expected "
                f"'{asset.digest}  {asset.name}'"
            )
        audited.append(asset.name)
    return tuple(audited)


def _read_url(
    url: str,
    *,
    headers: Mapping[str, str],
    limit: int,
    opener: Callable[..., Any] = urlopen,
    sleeper: Callable[[float], None] = time.sleep,
) -> bytes:
    request = Request(url, headers=dict(headers))
    for attempt in range(3):
        try:
            with opener(request, timeout=30) as response:
                payload = response.read(limit + 1)
            break
        except (HTTPError, URLError, TimeoutError, OSError) as error:
            if attempt == 2:
                raise FetchError(f"failed to fetch {url}: {error}") from error
            sleeper(2**attempt)
    if len(payload) > limit:
        raise FetchError(f"response from {url} exceeded {limit} bytes")
    return payload


def _github_api_headers(token: str | None) -> dict[str, str]:
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "NeverD-prebuilt-LLVM-audit",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def _fetch_json(url: str, token: str | None, description: str) -> object:
    payload = _read_url(
        url, headers=_github_api_headers(token), limit=API_RESPONSE_LIMIT
    )
    try:
        return json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FetchError(f"{description} is not valid JSON: {error}") from error


def _repository_api_url(pins: PinSet) -> str:
    owner, repository = pins.repository.split("/", 1)
    return (
        "https://api.github.com/repos/"
        f"{quote(owner, safe='')}/{quote(repository, safe='')}"
    )


def fetch_release(pins: PinSet, token: str | None) -> object:
    url = (
        f"{_repository_api_url(pins)}/releases/tags/"
        f"{quote(pins.tag, safe='')}"
    )
    return _fetch_json(url, token, "GitHub release metadata")


def fetch_tag_ref(pins: PinSet, token: str | None) -> object:
    url = (
        f"{_repository_api_url(pins)}/git/ref/tags/"
        f"{quote(pins.tag, safe='')}"
    )
    return _fetch_json(url, token, "GitHub tag reference metadata")


def fetch_sidecars(pins: PinSet, release: object) -> dict[str, str]:
    release_assets = _release_assets(release)
    sidecars = {}
    for asset in pins.assets:
        sidecar_name = f"{asset.name}.sha256"
        raw_sidecar = release_assets.get(sidecar_name)
        if raw_sidecar is None:
            raise AuditError(f"missing release asset: {sidecar_name}")
        url = raw_sidecar.get("browser_download_url")
        if not isinstance(url, str):
            raise AuditError(f"release asset {sidecar_name} has no download URL")
        parsed = urlparse(url)
        if parsed.scheme != "https" or parsed.hostname != "github.com":
            raise AuditError(f"release asset {sidecar_name} has an untrusted URL")
        payload = _read_url(url, headers={}, limit=CHECKSUM_RESPONSE_LIMIT)
        try:
            sidecars[sidecar_name] = payload.decode("ascii")
        except UnicodeDecodeError as error:
            raise AuditError(f"checksum sidecar {sidecar_name} is not ASCII") from error
    return sidecars


def create_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--consumer",
        type=Path,
        default=DEFAULT_CONSUMER,
        help="path to NeverDLLVMPrebuilt.cmake",
    )
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=ROOT,
        help="repository root containing the pinned LLVM submodule",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = create_argument_parser().parse_args(argv)
    try:
        pins = parse_pins(args.consumer.read_text(encoding="utf-8"))
        audit_gitlink(pins, read_gitlink(args.repository_root))
        token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
        release = fetch_release(pins, token)
        tag_ref = fetch_tag_ref(pins, token)
        sidecars = fetch_sidecars(pins, release)
        audited = audit_release(pins, release, tag_ref, sidecars)
    except AuditError as error:
        print(f"prebuilt LLVM release audit failed: {error}", file=sys.stderr)
        return 1
    except (FetchError, OSError) as error:
        print(f"prebuilt LLVM release audit could not run: {error}", file=sys.stderr)
        return 2

    print(
        f"prebuilt LLVM release valid: {pins.tag} at {pins.commit}; "
        f"{len(audited)} archives"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
