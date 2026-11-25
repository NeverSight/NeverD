"""Link the image storage declared by a single-function LLVM C fixture."""

import re

from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_RELOC_TYPE_PPC64


def llvmc_image_storage(source, original, section_name=".data"):
    # The public single-function LLVM C view declares its original image as
    # external storage. Supply the fixture bytes at the new link address.
    # On PPC64 the native linker also relocates its function descriptors.
    decl = re.search(r"/\* neverd\.image: (0x[0-9A-Fa-f]+) \(original loaded image storage\) \*/\n"
                     r"extern uint8_t (\w+)\[(\d+)\];", source.read_text())
    if not decl:
        return []
    base, name, size = int(decl[1], 16), decl[2], int(decl[3])
    with original.open("rb") as stream:
        elf = ELFFile(stream)
        data = elf.get_section_by_name(section_name)
        relocs = elf.get_section_by_name(".rela" + section_name)
        offset = data["sh_addr"] - base
        if not 0 <= offset <= size - data["sh_size"]:
            raise ValueError("LLVM C image declaration does not cover the fixture data")
        lines = [".data", ".balign 8", f".globl {name}", f"{name}:",
                 f".space {offset}", ".Lfixture_data:",
                 ".byte " + ", ".join(map(str, data.data())), f".org {name} + {size}"]
        for reloc in relocs.iter_relocations() if relocs else ():
            symbol = elf.get_section(relocs["sh_link"]).get_symbol(reloc["r_info_sym"])
            if (elf["e_machine"] != "EM_PPC64"
                    or reloc["r_info_type"] != ENUM_RELOC_TYPE_PPC64["R_PPC64_ADDR64"]
                    or symbol["st_info"]["type"] != "STT_FUNC"):
                raise ValueError("unexpected fixture data relocation")
            field = reloc["r_offset"] - data["sh_addr"]
            lines.extend([f".globl {symbol.name}", f".reloc .Lfixture_data + {field}, "
                          f"R_PPC64_ADDR64, {symbol.name} + {reloc['r_addend']}"])
    storage = source.with_suffix(".image.s")
    storage.write_text("\n".join(lines) + "\n")
    return [storage]
