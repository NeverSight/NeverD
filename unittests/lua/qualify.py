"""Qualify the checked-in, owned source corpus with the recorded compiler builds.

Usage: python3 qualify.py --tools DIR --neverd PATH
DIR contains lua-5.x.y/src/{lua,luac}, jit/src/luajit (and its jit modules),
and luau/build/{luau,luau-compile}. --record refreshes the fixture manifest.
Only repository-owned source or its reviewed recovered text is run. The
compiled chunks are passed exclusively to NeverD's offline reader.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "fixtures" / "dumps.json"
PUC = ["5.1.5", "5.2.4", "5.3.6", "5.4.8", "5.5.0"]
JIT = "3bfb8fa4ab970b36d42d108ead04e92341c49998"
LUAU = "f8a1e0129dce62c8c07ed5828781438e74452671"


def cells():
    for profile in [*PUC, "jit-W", "jit-X", "luau"]:
        for name in ["capture", "control", "iterator", "values"]:
            for debug in [True, False] if name == "capture" else [True]:
                yield profile, name, debug
        if profile == "luau":
            yield profile, "typed", True
        if profile in PUC:
            yield profile, "effects", True
        if profile in PUC[2:]:
            yield profile, "integers", True
        if profile in PUC[3:]:
            yield profile, "finalizer", True


def compile_fixture(tools, tmp, profile, name, debug):
    source = HERE / "fixtures" / (name + ".lua")
    chunk = tmp / "input.bin"
    if profile in PUC:
        bindir = tools / ("lua-" + profile) / "src"
        cmd = [bindir / "luac", *([] if debug else ["-s"]), "-o", chunk, "-"]
        subprocess.run(cmd, input=source.read_bytes(), check=True)
        return chunk, bindir / "lua"
    if profile == "luau":
        bindir = tools / "luau" / "build"
        cmd = [bindir / "luau-compile", "--binary", "-O0", "-t1" if name == "typed" else "-t0", "-g2" if debug else "-g0", source.name]
        chunk.write_bytes(subprocess.check_output(cmd, cwd=source.parent))
        return chunk, bindir / "luau"
    bindir = tools / "jit" / "src"
    cmd = [bindir / "luajit", "-b" + ("g" if debug else "s") + profile[-1],
           "-t", "raw", source.name, chunk]
    subprocess.run(cmd, cwd=source.parent, env={**os.environ, "LUA_PATH": str(bindir / "?.lua")}, check=True)
    return chunk, bindir / "luajit"


def observe(tmp, runtime, source):
    # Capture result count, types, every string byte and trailing nils.
    runner = tmp / "observe.lua"
    runner.write_text("""local function observe(...)
  print(select("#", ...))
  for i = 1, select("#", ...) do
    local v = select(i, ...)
    if type(v) == "string" then
      local bytes = {}
      for j = 1, #v do bytes[j] = string.byte(v, j) end
      print("string", table.concat(bytes, ","))
    else print(type(v), tostring(v)) end
  end
end
local function fixture(...)
""" + source + "\nend\nobserve(fixture(6, 3))\n")
    return subprocess.check_output([runtime, runner], timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, required=True)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    profiles = {v: "Lua " + v for v in PUC}
    profiles.update({"jit-W": JIT, "jit-X": JIT, "luau": "0.659@" + LUAU})
    result = {"compilers": profiles, "fixtures": []}
    existing = {} if args.record else {e["id"]: e for e in json.loads(MANIFEST.read_text())["fixtures"]}
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        for profile, name, debug in cells():
            key = f"{profile}/{name}/" + ("debug" if debug else "stripped")
            chunk, runtime = compile_fixture(args.tools.resolve(), tmp, profile, name, debug)
            data = chunk.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            if not args.record and digest != existing[key]["sha256"]:
                raise ValueError(key + ": compiler output differs from recorded fixture")
            dialect = "luau-0.659" if profile == "luau" else "auto"
            command = [args.neverd.resolve(), "lua", "--json", "--profile=" + dialect, chunk]
            report = json.loads(subprocess.check_output(command))
            fn = next(f for f in report["functions"] if f["id"] == report["root"])
            assert report["status"] == "reconstructed_source", (key, fn["diagnostics"])
            source = (HERE / "fixtures" / (name + ".lua")).read_text()
            expected = observe(tmp, runtime, source)
            actual = observe(tmp, runtime, fn["source"])
            if actual != expected:
                raise ValueError(f"{key}: {actual!r} != {expected!r}")
            result["fixtures"].append({"id": key, "profile": dialect, "sha256": digest,
                                        "bytes": base64.b64encode(data).decode("ascii")})
    if args.record:
        MANIFEST.write_text(json.dumps(result, indent=2) + "\n")
    elif result != json.loads(MANIFEST.read_text()):
        raise ValueError("Fixture manifest differs")
    print(f"{len(result['fixtures'])} dialect-matched behavioral comparisons passed")


if __name__ == "__main__":
    main()
