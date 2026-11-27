"""Qualify standalone fork LLD against the two published SDKs on native hosts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from zipfile import ZipFile


HEAD = "f81e1d7fb33b543dee6a8ad8a7abafccf9e9c4a6"
FORK = {"r14": "6c769154fbe9db2fe9e2e2dc1b04bebfe64e1312", "r15": "54143a8d7c037ee4c22f1f238bd4d50ca9a5fd00"}
RUN = 45916589182
SDK = {
    "Linux": ("neverd-llvm-linux-x86_64", ".tar.xz", "8bdcf7289ada160858c35ccb6dbf848c46c75161063c65b5653954b3e4d4d2bd", "527eb1cc928ef53316d79ea3fb986719a8334a6f62a1f59238a48aad289c7b3f"),
    "macOS": ("neverd-llvm-macos-arm64", ".tar.xz", "b8e70c79d2a44ba0dfd49803fa958b6408037960930fbeb2cb5f0bc60b98ac54", "fab08ae7c4dd67263c1180f211f928d0612cc828bd0a67942f5c0c4f0f099b97"),
    "Windows": ("neverd-llvm-windows-x64", ".zip", "2f23b21740c23036f33c7cf6d5cc19486b9d6cf25bc882795f86dd41482f2184", "e572879e578b074de06e0b51d6f7148e4d4cbeec01cae526478a2386e8b001d9"),
}
CORPUS = {
    "Linux": (15164941802, "06fe4a547fe3c60b060d00558d8883c759832ad7594b634b196c21cef37f9f92"),
    "macOS": (15166110454, "e019d7d658081a07c24f51f35736c07bc70951fd8fdc1b0c687373cd113094ae"),
    "Windows": (15165883181, "f62e94196f2cfd57d0c2cadb57f1c851a1cee12223ac77ef5041c5f68a3eca88"),
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def capture(argv):
    return subprocess.check_output(argv, text=True).strip()


def run(argv, **options):
    subprocess.run(argv, check=True, **options)


def cache_value(build, name):
    line, = [x for x in (build / "CMakeCache.txt").read_text().splitlines() if x.startswith(name + ":")]
    return line.split("=", 1)[1]


def fetch_corpus(host):
    aid, expected = CORPUS[host]
    endpoint = f"repos/NeverSight/NeverD/actions/artifacts/{aid}"
    meta = json.loads(capture(["gh", "api", endpoint]))
    assert meta["workflow_run"]["id"] == RUN and meta["workflow_run"]["head_sha"] == HEAD
    assert meta["digest"] == "sha256:" + expected and not meta["expired"]
    with Path("corpus.zip").open("wb") as stream:
        run(["gh", "api", endpoint + "/zip"], stdout=stream)
    assert digest(Path("corpus.zip")) == expected


def build_drivers(host, project, source):
    suffix = ".exe" if host == "Windows" else ""
    root = Path.cwd()
    pbuild = project / "build-sdk"
    assert capture(["git", "-C", project, "rev-parse", "HEAD"]) == HEAD
    assert capture(["git", "-C", project, "rev-parse", "HEAD:third_party/llvm-project"]) == FORK["r15"]
    assert capture(["git", "-C", source, "rev-parse", "HEAD"]) == FORK["r15"]
    run(["git", "-C", source, "diff", "--exit-code", FORK["r14"], FORK["r15"], "--",
         "lld", "cmake", "llvm/include", "llvm/cmake", "llvm/utils/TableGen", "llvm/lib/TableGen", "libunwind/include"])
    name, extension, old_hash, new_hash = SDK[host]
    archive_name = name + extension
    current = Path(cache_value(pbuild, "LLVM_DIR")).parents[2]
    assert cache_value(pbuild, "NEVERD_LLVM_PREBUILT_TAG") == "neverd-llvm-v23.0.0-r15"
    assert current.name == name and digest(current.parent / archive_name) == new_hash
    old_root = root / "sdk-r14"
    old_root.mkdir(exist_ok=True)
    older = old_root / archive_name
    url = "https://github.com/NeverSight/llvm-project/releases/download/neverd-llvm-v23.0.0-r14/" + archive_name
    run(["curl", "--fail", "--location", "--retry", "3", "--silent", "--show-error", "-o", older, url])
    assert digest(older) == old_hash
    run(["cmake", "-E", "tar", "xf", older], cwd=old_root)
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DLLVM_ENABLE_RTTI=ON", "-DLLVM_INCLUDE_TESTS=OFF"]
    if host == "Linux":
        common += ["-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14", "-DLLVM_USE_LINKER=lld"]
    gen = root / "build-tablegen"
    run(["cmake", "-S", source / "llvm", "-B", gen, *common, "-DLLVM_TARGETS_TO_BUILD=Mips",
         "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_ENABLE_ZLIB=OFF",
         "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF"])
    run(["cmake", "--build", gen, "--parallel", "4", "--target", "llvm-tblgen"])
    generator = gen / "bin" / ("llvm-tblgen" + suffix)
    tooling = dict(tablegen=digest(generator), project=HEAD, source=FORK)
    for label, sdk, sha in [("r14", old_root / name, old_hash), ("r15", current, new_hash)]:
        info = (sdk / "BUILDINFO.txt").read_text()
        assert f"llvm_commit: {FORK[label]}" in info and "X86;ARM;AArch64;Mips" in info
        config = sdk / "lib/cmake/llvm"
        build = root / ("build-lld-" + label)
        run(["cmake", "-S", source / "lld", "-B", build, *common,
             "-DLLVM_DIR=" + config.as_posix(), "-DLLVM_CMAKE_DIR=" + config.as_posix(),
             "-DLLVM_TABLEGEN_EXE=" + generator.as_posix(), "-DLLVM_PARALLEL_LINK_JOBS=1"])
        assert Path(cache_value(build, "LLVM_DIR")).resolve() == config.resolve()
        run(["cmake", "--build", build, "--parallel", "4", "--target", "lld"])
        driver = root / "drivers" / label / ("ld.lld" + suffix)
        driver.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / "bin" / ("lld" + suffix), driver)
        version = capture([driver, "--version"])
        assert f"https://github.com/NeverSight/llvm-project {FORK[label]}" in version
        tooling[label] = dict(sdk_archive=sha, sdk_buildinfo=info, linker=digest(driver), version=version)
    (root / "drivers/source.json").write_text(json.dumps(tooling, indent=2) + "\n")


def check(host, project, archive, cli, old, new, out):
    sys.path.insert(0, str(project / "scripts"))
    import check_mips_elf_relink as m

    assert digest(archive) == CORPUS[host][1]
    for tag, driver in (("r14", old), ("r15", new)):
        assert f"https://github.com/NeverSight/llvm-project {FORK[tag]}" in capture([driver, "--version"])
    out.mkdir(exist_ok=True)
    cases = {}
    with ZipFile(archive) as z, TemporaryDirectory() as temp:
        for model, group in (("static", "from-r14-partial"), ("pic", "from-r14-partial-pic")):
            source_products = json.loads(z.read(group + "/products.json"))
            stage = out / model
            stage.mkdir()
            result = {}
            for p in m.profiles(1):
                d = Path(temp) / model / p.name
                d.mkdir(parents=True)
                saved = stage / p.name
                saved.mkdir()
                prefix = group + "/" + p.name + "/"
                for obj in ("calc.o", "updated.o"):
                    (d / obj).write_bytes(z.read(prefix + obj))
                    m.verify_partial_object(d / obj, p)
                    shutil.copy2(d / obj, saved / obj)
                original = saved / "original.elf"
                original.write_bytes(z.read(prefix + "original.elf"))
                hashes = {"original": digest(original)}
                assert hashes["original"] == source_products[p.name]["original"]
                m.write(d / "base.ld", m.layout(p, False))
                m.write(d / "section.ld", m.layout(p, True))
                m.encode(d / "manifest.json", dict(objects=["calc.o"], original="calc.o", replacement="updated.o",
                         function="calc", layout="base.ld", section=".calc.update", sectionLayout="section.ld"))
                direct = d / "direct"
                direct.mkdir()
                shutil.copy2(d / "updated.o", direct / "calc.o")
                for mode in m.MODES:
                    output = saved / (mode + ".elf")
                    cli_args = [cli, "patch", original, "--relink=" + str(d / "manifest.json"), "--mode=" + mode,
                                "--linker-path=" + str(new), "-o", output]
                    output.write_bytes(b"previous output\n")
                    m.run(cli_args, error="do not reproduce")
                    assert output.read_bytes() == b"previous output\n"
                    m.run([*cli_args, "--original-linker-path=" + str(old)])
                    layout = d / ("section.ld" if mode == "section" else "base.ld")
                    m.run([new, "-static", "-T", layout, "-o", d / "reference.elf", direct / "calc.o"])
                    assert output.read_bytes() == (d / "reference.elf").read_bytes() == z.read(prefix + mode + ".elf")
                    hashes[mode] = digest(output)
                assert hashes == source_products[p.name]
                result[p.name] = hashes
            assert len(result) == 6
            m.encode(stage / "products.json", result)
            cases[model] = result
    m.encode(out / "comparison.json", dict(host=host, source=FORK, project=HEAD, cli=digest(cli), inputs=CORPUS[host],
             drivers={"r14": digest(old), "r15": digest(new)}, cases=cases, originals=12, patched=24, rejected=24))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("fetch", "build", "check"))
    parser.add_argument("--project", type=Path, default=Path("project"))
    parser.add_argument("--source", type=Path, default=Path("llvm-src"))
    parser.add_argument("--host", choices=SDK, default=os.environ.get("RUNNER_OS"))
    parser.add_argument("--archive", type=Path, default=Path("corpus.zip"))
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--old", type=Path)
    parser.add_argument("--new", type=Path)
    parser.add_argument("--out", type=Path, default=Path("products"))
    args = parser.parse_args()
    project = args.project.resolve()
    if args.operation == "fetch":
        fetch_corpus(args.host)
    elif args.operation == "build":
        build_drivers(args.host, project, args.source.resolve())
    else:
        suffix = ".exe" if os.name == "nt" else ""
        cli = args.cli or project / "build-sdk/bin" / ("neverd" + suffix)
        old = args.old or Path("drivers/r14") / ("ld.lld" + suffix)
        new = args.new or Path("drivers/r15") / ("ld.lld" + suffix)
        check(args.host, project, args.archive, cli.resolve(), old.resolve(), new.resolve(), args.out.resolve())


if __name__ == "__main__":
    main()
