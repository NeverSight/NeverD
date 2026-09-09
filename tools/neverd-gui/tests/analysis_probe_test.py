#!/usr/bin/env python3
"""Exercise actual QML/worker views on a self-contained, non-executed ELF."""
from pathlib import Path
import ctypes
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "neverd-worker/tests"))
from native_mapping_test import native_elf


def analysis_fixture():
    """Add a real function symbol so normal GUI navigation discovers the entry.

    The worker mapping test deliberately uses a stripped image and calls its
    entry address directly. The desktop probe instead starts from the loader's
    function model, so its fixture needs an allocated .text section and STT_FUNC.
    Keep the high virtual address to exercise lossless GUI address transport.
    """
    base = 0xffff800000400000
    image, entry = native_elf(True, base)
    data = bytearray(image)
    code_offset = entry - base
    code_size = len(image) - code_offset
    names = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    strings = b"\0main\0"

    def append(blob, alignment=1):
        data.extend(bytes((-len(data)) % alignment))
        offset = len(data)
        data.extend(blob)
        return offset

    strings_offset = append(strings)
    names_offset = append(names)
    # Elf64_Sym: null/local symbol followed by global function in section 1.
    symbols = bytes(24) + struct.pack("<IBBHQQ", 1, 0x12, 0, 1, entry, code_size)
    symbols_offset = append(symbols, 8)
    section_offset = append(b"", 8)

    def section(name, kind, flags=0, address=0, offset=0, size=0,
                link=0, info=0, alignment=1, entry_size=0):
        return struct.pack("<IIQQQQIIQQ", names.index(name) if name else 0, kind,
                           flags, address, offset, size, link, info, alignment, entry_size)

    data.extend(bytes(64))
    data.extend(section(b".text\0", 1, 6, entry, code_offset, code_size, alignment=4))
    data.extend(section(b".symtab\0", 2, offset=symbols_offset, size=len(symbols),
                        link=3, info=1, alignment=8, entry_size=24))
    data.extend(section(b".strtab\0", 3, offset=strings_offset, size=len(strings)))
    data.extend(section(b".shstrtab\0", 3, offset=names_offset, size=len(names)))
    struct.pack_into("<Q", data, 40, section_offset)
    struct.pack_into("<HHH", data, 58, 64, 5, 4)
    return bytes(data)


def main():
    # The desktop supports older engines with explicit unavailable mapping;
    # this particular test requires the additive API from the matching SDK.
    engine = ctypes.CDLL(sys.argv[2])
    if not hasattr(engine, "neverd_ir_view_json"):
        print("SKIP: engine predates mapped analysis pages")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-gui-analysis-") as directory:
        path = Path(directory) / "native.elf"
        path.write_bytes(analysis_fixture())
        return subprocess.run([sys.argv[1], "--analysis-test", str(path)], timeout=25).returncode


if __name__ == "__main__":
    raise SystemExit(main())
