#!/usr/bin/env python3
"""External ground-truth dumper for NeverD bench comparison.

Drives the configured reference analyzer CLI (see ND_REF_CLI) and emits:
  <stem>.ref.functions.json -- [{addr, name, size}]
  <stem>.ref.imports.json   -- [{dll, symbol, iat_addr}]
  <stem>.ref.strings.json   -- [{addr, encoding, content, length}]
  <stem>.ref.timings.json   -- {analyze_ms, dump_ms, total_ms, ...}

Cold-start timing: by default we wipe the analyzer's on-disk cache for the
target so the first call triggers full analysis. Pass --warm to skip the wipe.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Backend paths are resolved from the environment so the bench harness stays
# agnostic to whichever external analyzer is installed locally.
REF_HOME_CACHE = Path(os.environ.get("ND_REF_CACHE", Path.home() / ".ida" / "idb"))
REF_CLI = os.environ.get(
    "ND_REF_CLI",
    os.environ.get("ND_IDA_CLI", str(Path.home() / ".local/bin/ida-cli")),
)
REF_SOCK_CANDIDATES = (
    Path.home() / ".ida" / "server.sock",
    Path.home() / ".ida" / "server.pid",
    Path.home() / ".ida" / ".startup.lock",
    Path("/tmp/ida-cli.socket"),
)
REF_KILL_PATTERNS = ("idat", "ida-cli")


def _strip_banner(out: str) -> str:
    """Trim everything before the first { or [ and after the matching close.
    Some CLIs wrap responses in a banner line and a trailing thank-you note."""
    start = -1
    for i, ch in enumerate(out):
        if ch in "{[":
            start = i
            break
    if start < 0:
        return out
    s = out[start:]
    open_ch = s[0]
    close_ch = "}" if open_ch == "{" else "]"
    depth = 0
    in_str = False
    esc = False
    end = -1
    for i, ch in enumerate(s):
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            continue
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end < 0:
        return s
    return s[:end]


def run_cli(binary: Path, sub: list, timeout: int = 1800) -> dict:
    """Run `<ref-cli> cli --json --path <bin> <sub...>` and parse JSON result.

    The backend's per-request default is often too short for cold analysis of
    large (>30MB) binaries. Pass --timeout to lift it.
    """
    cmd = [REF_CLI, "cli", "--json", "--timeout", str(timeout), "--path", str(binary)] + sub
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 60)
    if proc.returncode != 0:
        raise RuntimeError(
            f"ref-cli failed (rc={proc.returncode}): {' '.join(sub)}\n"
            f"stderr: {proc.stderr}\nstdout: {proc.stdout[-2000:]}"
        )
    out = _strip_banner(proc.stdout)
    try:
        return json.loads(out)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"unparseable JSON from {' '.join(sub)}: {e}\n{out[:2000]}")


def run_raw(binary: Path, method: str, params: dict | None = None,
            timeout: int = 900) -> object:
    """Call the JSON-RPC `method` via `cli raw`. Resolves cached oversize responses."""
    p = dict(params or {})
    p.setdefault("path", str(binary))
    payload = json.dumps({"method": method, "params": p})
    out = run_cli(binary, ["raw", payload], timeout=timeout)
    if isinstance(out, dict) and out.get("cached") and out.get("file"):
        try:
            with open(out["file"]) as f:
                return json.load(f)
        except Exception as e:
            raise RuntimeError(f"failed to read cached file {out['file']}: {e}")
    return out


def _parse_addr(s):
    if isinstance(s, int):
        return s
    if not s:
        return 0
    s = str(s)
    if s.lower().startswith("0x"):
        return int(s, 16)
    try:
        return int(s)
    except ValueError:
        return int(s, 16)


def shutdown_server() -> None:
    """Shutdown any running ref-cli server so cache wipes don't conflict with open workers."""
    try:
        subprocess.run([REF_CLI, "cli", "shutdown"], capture_output=True, text=True, timeout=20)
    except Exception:
        pass
    # Force-kill stragglers and stale socket/pid files; otherwise the next
    # client call may get "Cannot connect to server: Connection refused".
    try:
        for pat in REF_KILL_PATTERNS:
            subprocess.run(["pkill", "-KILL", "-f", pat], capture_output=True, timeout=10)
    except Exception:
        pass
    for p in REF_SOCK_CANDIDATES:
        try:
            if p.exists():
                p.unlink()
        except OSError:
            pass


def wipe_cache(binary: Path) -> None:
    """Remove cached analyzer databases so analysis is cold.

    The backend caches databases by content hash, so byte-identical binaries
    (e.g. llvm-readelf.exe / llvm-readobj.exe in this repo) share a cache
    entry. A stem-based wipe doesn't catch that. Wipe everything to be
    safe — the cache rebuilds on next open.
    """
    shutdown_server()
    if not REF_HOME_CACHE.exists():
        return
    for p in list(REF_HOME_CACHE.iterdir()):
        if p.name == "index.json" or p.name.endswith(".i64") or p.name.endswith(".idb"):
            try:
                if p.is_file():
                    p.unlink()
                else:
                    shutil.rmtree(p, ignore_errors=True)
            except OSError:
                pass


def dump_functions(binary: Path) -> list[dict]:
    funcs = []
    offset = 0
    page = 2000
    while True:
        resp = run_cli(binary, ["list-functions", "--limit", str(page), "--offset", str(offset)])
        if isinstance(resp, dict) and resp.get("cached"):
            with open(resp["file"]) as f:
                resp = json.load(f)
        items = resp.get("functions") if isinstance(resp, dict) else resp
        if not items:
            break
        for it in items:
            addr = _parse_addr(it.get("address") or it.get("start_ea") or it.get("addr"))
            name = it.get("name") or ""
            size = int(it.get("size") or 0)
            funcs.append({"addr": addr, "name": name, "size": size})
        total = resp.get("total") if isinstance(resp, dict) else None
        if total is not None and len(funcs) >= total:
            break
        if len(items) < page:
            break
        offset += page
    return funcs


def dump_imports(binary: Path) -> list[dict]:
    raw = run_raw(binary, "list_imports")
    items = raw if isinstance(raw, list) else (raw.get("result") if isinstance(raw, dict) else [])
    if not isinstance(items, list):
        items = []
    out = []
    for it in items:
        if not isinstance(it, dict):
            continue
        ea = _parse_addr(it.get("address") or it.get("ea") or 0)
        sym = it.get("name") or it.get("symbol") or ""
        dll = it.get("module") or it.get("dll") or ""
        ordn = int(it.get("ordinal") or 0)
        out.append({"dll": dll, "symbol": sym, "iat_addr": ea, "ordinal": ordn})
    return out


def dump_strings(binary: Path) -> list[dict]:
    resp = run_cli(binary, ["list-strings", "--limit", "1000000"])
    if isinstance(resp, dict) and resp.get("cached"):
        with open(resp["file"]) as f:
            resp = json.load(f)
    items = resp.get("strings") if isinstance(resp, dict) else resp
    if not isinstance(items, list):
        items = []
    out = []
    for it in items:
        addr = _parse_addr(it.get("address") or it.get("ea") or it.get("addr"))
        content = it.get("content") or it.get("value") or it.get("string") or ""
        enc = it.get("encoding") or it.get("type") or "ascii"
        length = int(it.get("length") or len(content))
        out.append({"addr": addr, "encoding": str(enc), "content": content, "length": length})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--warm", action="store_true",
                    help="don't wipe ref cache (reuse prior database)")
    ap.add_argument("--timeout-s", type=int, default=900)
    args = ap.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.exists():
        sys.exit(f"binary not found: {binary}")

    out_dir = Path(args.out_dir) if args.out_dir else binary.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = binary.stem

    if not args.warm:
        wipe_cache(binary)

    timings = {"cold_start": not args.warm}

    t0 = time.monotonic()
    _ = run_cli(binary, ["list-functions", "--limit", "1", "--offset", "0"],
                timeout=args.timeout_s)
    analyze_ms = int((time.monotonic() - t0) * 1000)
    timings["analyze_ms"] = analyze_ms

    t1 = time.monotonic()
    funcs = dump_functions(binary)
    imports = dump_imports(binary)
    strings = dump_strings(binary)
    dump_ms = int((time.monotonic() - t1) * 1000)
    timings["dump_ms"] = dump_ms
    timings["total_ms"] = analyze_ms + dump_ms
    timings["num_functions"] = len(funcs)
    timings["num_imports"] = len(imports)
    timings["num_strings"] = len(strings)

    base = out_dir / stem
    with open(f"{base}.ref.functions.json", "w") as f:
        json.dump(funcs, f, indent=1)
    with open(f"{base}.ref.imports.json", "w") as f:
        json.dump(imports, f, indent=1)
    with open(f"{base}.ref.strings.json", "w") as f:
        json.dump(strings, f, indent=1)
    with open(f"{base}.ref.timings.json", "w") as f:
        json.dump(timings, f, indent=2)

    print(f"[{binary.name}] funcs={len(funcs)} imports={len(imports)} "
          f"strings={len(strings)} analyze_ms={analyze_ms} dump_ms={dump_ms}")


if __name__ == "__main__":
    main()
