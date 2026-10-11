"""Checked executable owners and SafeSEH storage shared by PE32 runtime probes."""
from __future__ import annotations

import hashlib
import struct

if __package__:
    from .check_windows_registration_rewrite import PE32
else:
    from check_windows_registration_rewrite import PE32


def code_owner(image: PE32, begin: int, end: int, name: str) -> None:
    if not 0 <= begin < end <= 0xffffffff:
        raise ValueError("C++ code owner has an invalid range")
    owners = [section for section in image.sections
              if section[1] <= begin and end <= section[1] + section[2]]
    if len(owners) != 1 or owners[0][0] != name:
        raise ValueError("C++ code owner has no unique executable section")
    image.raw(begin, end - begin)
    section_table = image.optional + image.u16(image.optional - 4)
    index = image.sections.index(owners[0])
    flags = image.u32(section_table + 40 * index + 36)
    if flags & 0x60000000 != 0x60000000 or flags & 0x80000000:
        raise ValueError("C++ code section lost read/execute permissions")


def safe_handlers(image: PE32) -> tuple[int, list[int]]:
    rva, size = image.directory(10)
    if not rva or size < 4:
        raise ValueError("C++ input has no load configuration")
    config = image.raw(rva, size)
    declared = image.u32(config)
    if declared < 72:
        raise ValueError("C++ load configuration has no complete SafeSEH fields")
    config = image.raw(rva, declared)
    table, count = image.u32(config + 64), image.u32(config + 68)
    if not table or not 0 < count <= 65536 or table < image.base:
        raise ValueError("C++ SafeSEH table is incomplete")
    offset = image.raw(table - image.base, count * 4)
    handlers = [image.u32(offset + 4 * i) for i in range(count)]
    if handlers != sorted(set(handlers)):
        raise ValueError("C++ SafeSEH table is not strictly ordered")
    for handler in handlers:
        owners = [s for s in image.sections if s[1] <= handler < s[1] + s[2]]
        if len(owners) != 1 or owners[0][0] not in (".text", ".ndtext"):
            raise ValueError("C++ SafeSEH handler has no executable owner")
        code_owner(image, handler, handler + 1, owners[0][0])
    return rva + 64, handlers



def jump_target(image, entry):
    offset = image.raw(entry, 5)
    if image.data[offset] != 0xe9:
        raise ValueError("re-lifted entry lost its exact trampoline")
    return (entry + 5 + struct.unpack_from("<i", image.data, offset + 1)[0]) & 0xffffffff


def validate_generation(original, product, receipt, first, export):
    if receipt.get("schema") != 1 or \
            receipt.get("evidence") != "checked-realigned-source-reconstruction" or \
            receipt.get("source_frame") != "realigned" or \
            receipt.get("base") != 0x400000 or original.base != 0x400000 or \
            product.base != 0x400000 or \
            receipt.get("source_image_sha256") != hashlib.sha256(original.data).hexdigest() or \
            receipt.get("image_sha256") != hashlib.sha256(product.data).hexdigest():
        raise ValueError("re-lifted installation lost its image or frame proof")
    if (receipt["source_begin"], receipt["source_end"]) != \
            (first["generated_begin"], first["generated_end"]) or \
            receipt["source_image_sha256"] != first["image_sha256"]:
        raise ValueError("re-lifted source is not the proved first generation")
    entry = original.entry(export)
    if product.entry(export) != entry or entry != first["source_begin"] or \
            jump_target(original, entry) != receipt["source_begin"] or \
            jump_target(product, entry) != receipt["source_begin"] or \
            jump_target(product, receipt["source_begin"]) != receipt["generated_begin"] or \
            receipt["generated_begin"] < receipt["source_end"]:
        raise ValueError("two-generation trampoline chain changed its source or destination")
    code_owner(original, receipt["source_begin"], receipt["source_end"], ".ndtext")
    code_owner(product, receipt["generated_begin"], receipt["generated_end"], ".ndtext")

