import json
from pathlib import Path
import shutil
import struct
import subprocess
import platform

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "observations"
OUT.mkdir(exist_ok=True)
(OUT / "platform.json").write_text(json.dumps(dict(platform=platform.platform(),
                                                version=platform.version()), indent=2) + "\n")
CLANG = shutil.which("clang")
LINK = shutil.which("lld-link")
if not CLANG or not LINK:
    raise SystemExit("Clang and lld-link are required")
FLAGS = ["--target=x86_64-pc-windows-msvc", "-std=c11", "-ffreestanding",
         "-fno-builtin", "-fno-stack-protector", "-fno-vectorize",
         "-fno-slp-vectorize", "-O1", "-c", str(ROOT / "probe.c")]
subprocess.run([CLANG, *FLAGS, "-DPROBE_DLL", "-o", str(OUT / "input.obj")],
               check=True)
apis = ["LoadLibraryExA", "FreeLibrary", "VirtualQuery", "ReadProcessMemory",
        "GetCurrentProcess", "GetLastError", "GetStdHandle", "WriteFile",
        "ExitProcess"]
(OUT / "kernel.def").write_text("LIBRARY kernel32.dll\nEXPORTS\n" +
                                "\n".join(apis) + "\n")
subprocess.run([LINK, "/lib", "/machine:x64", "/def:" + str(OUT / "kernel.def"),
                "/out:" + str(OUT / "kernel.lib")], check=True)
for mode, load_flags in [("native", 0), ("native-unresolved", 1)]:
    subprocess.run([CLANG, *FLAGS, "-DPROBE_LOAD_FLAGS=" + str(load_flags),
                    "-o", str(OUT / (mode + ".obj"))], check=True)
    subprocess.run([LINK, "/nodefaultlib", "/subsystem:console", "/machine:x64",
                    "/entry:hostEntry", str(OUT / (mode + ".obj")),
                    str(OUT / "kernel.lib"), "/out:" + str(OUT / (mode + ".exe"))],
                   check=True)
cases = [
    ("page-full", 4096, 512, 8192, 8192),
    ("page-raw-tail", 4096, 512, 0x100, 8192),
    ("page-one-page", 4096, 512, 4096, 4096),
    ("page-zero-virtual", 4096, 512, 0, 8192),
    ("large-full", 65536, 512, 8192, 8192),
    ("large-raw-tail", 65536, 512, 0x100, 8192),
    ("large-one-page", 65536, 512, 4096, 4096),
    ("large-bss", 65536, 65536, 4096, 0),
    ("large-raw-padding", 65536, 65536, 0x100, 65536),
]
metadata = []
for name, section_align, file_align, virtual_size, raw_size in cases:
    path = OUT / (name + ".dll")
    subprocess.run([LINK, "/dll", "/nodefaultlib", "/subsystem:console",
                    "/machine:x64", "/entry:dllEntry", "/include:ProbeData",
                    "/align:" + str(section_align),
                    "/filealign:" + str(file_align), str(OUT / "input.obj"),
                    "/out:" + str(path)], check=True)
    data = bytearray(path.read_bytes())
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    for index in range(count):
        at = table + index * 40
        if data[at:at + 8].rstrip(b"\0") == b".probe":
            rva, original_raw, offset = struct.unpack_from("<III", data, at + 12)
            assert raw_size <= original_raw
            struct.pack_into("<I", data, at + 8, virtual_size)
            struct.pack_into("<I", data, at + 16, raw_size)
            if not raw_size:
                struct.pack_into("<I", data, at + 20, 0)
            metadata.append(dict(case=name + ".dll", section_alignment=section_align,
                                 file_alignment=file_align, virtual_size=virtual_size,
                                 raw_size=raw_size, rva=rva, file_offset=offset,
                                 file_markers={hex(i): data[offset + i] for i in [0x100, 0xfff, 0x1000, 0x1fff]}))
            break
    else:
        raise RuntimeError("missing probe section")
    path.write_bytes(data)
(OUT / "cases.json").write_text(json.dumps(metadata, indent=2) + "\n")
for mode in ["native", "native-unresolved"]:
    run = subprocess.run([str(OUT / (mode + ".exe"))], cwd=OUT, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (OUT / (mode + ".log")).write_text(run.stdout)
    print(mode + ":")
    print(run.stdout)
    records = []
    for line in run.stdout.splitlines():
        record = {}
        for field in line.split():
            key, value = field.split("=", 1)
            record[key] = int(value, 16) if value.startswith("0x") else value
        records.append(record)
    (OUT / (mode + ".json")).write_text(json.dumps(records, indent=2) + "\n")
    reported = [record["case"] for record in records if "loaded" in record]
    if run.returncode or set(reported) != {case[0] + ".dll" for case in cases}:
        raise SystemExit(mode + " probe did not report every case")
