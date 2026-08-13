//===- SectionNames.h - Well-known section/segment names -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Centralised string constants for well-known section and segment names
/// across COFF, ELF, and Mach-O.  Included by both loader and codegen
/// layers so that name comparisons are never done against raw string
/// literals scattered through the codebase.
///
/// Modelled after LLVM's llvm/BinaryFormat/{COFF,ELF,MachO}.h for
/// constant management.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_OBJECT_SECTIONNAMES_H
#define NEVERD_OBJECT_SECTIONNAMES_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

namespace neverd {
namespace section_names {

namespace elf {
constexpr const char *Text = ".text";
// Function/section split produced by -ffunction-sections and hot/cold
// splitting: ".text.hot", ".text.unlikely", ".text.<symbol>", ...
constexpr const char *TextSplitPrefix = ".text.";
constexpr const char *Data = ".data";
constexpr const char *DataSplitPrefix = ".data.";
constexpr const char *DataRelRo = ".data.rel.ro";
constexpr const char *Bss = ".bss";
constexpr const char *BssSplitPrefix = ".bss.";
constexpr const char *Rodata = ".rodata";
constexpr const char *RelaPlt = ".rela.plt";
constexpr const char *RelPlt = ".rel.plt";
constexpr const char *Plt = ".plt";
constexpr const char *PltPrefix = ".plt.";
constexpr const char *PltGot = ".plt.got";
constexpr const char *Iplt = ".iplt";
constexpr const char *EhFrameHdr = ".eh_frame_hdr";
constexpr const char *EhFrame = ".eh_frame";
constexpr const char *GccExceptTable = ".gcc_except_table";
constexpr const char *ArmExIdx = ".ARM.exidx";
constexpr const char *ArmExTab = ".ARM.extab";
// The linker keeps a per-input-section index and table when it is told not to
// merge them, which is what `-ffunction-sections` without `--merge-exidx-
// entries` produces: ".ARM.exidx.text.<symbol>", ".ARM.extab.text.<symbol>".
constexpr const char *ArmExIdxPrefix = ".ARM.exidx.";
constexpr const char *ArmExTabPrefix = ".ARM.extab.";
constexpr const char *GoPclnTab = ".gopclntab";
constexpr const char *GoSymTab = ".gosymtab";
constexpr const char *GoBuildInfo = ".go.buildinfo";
constexpr const char *NoPtrData = ".noptrdata";
constexpr const char *Dynamic = ".dynamic";
constexpr const char *Got = ".got";
constexpr const char *GotPlt = ".got.plt";
constexpr const char *Dynsym = ".dynsym";
constexpr const char *Dynstr = ".dynstr";
constexpr const char *Init = ".init";
constexpr const char *Fini = ".fini";
constexpr const char *PreinitArray = ".preinit_array";
constexpr const char *InitArray = ".init_array";
constexpr const char *FiniArray = ".fini_array";
constexpr const char *Ctors = ".ctors";
constexpr const char *Dtors = ".dtors";
constexpr const char *Interp = ".interp";
constexpr const char *Note = ".note";
constexpr const char *Symtab = ".symtab";
constexpr const char *Strtab = ".strtab";
constexpr const char *Shstrtab = ".shstrtab";
constexpr const char *TLS = ".tls";
constexpr const char *TBss = ".tbss";
constexpr const char *TData = ".tdata";
constexpr const char *DebugInfo = ".debug_info";
} // namespace elf

namespace coff {
constexpr const char *Text = ".text";
constexpr const char *TextPrefix = ".text$";
constexpr const char *TextMn = ".text$mn";
constexpr const char *Data = ".data";
constexpr const char *Rdata = ".rdata";
constexpr const char *Bss = ".bss";
constexpr const char *Pdata = ".pdata";
constexpr const char *Xdata = ".xdata";
constexpr const char *Reloc = ".reloc";
constexpr const char *Idata = ".idata";
constexpr const char *Edata = ".edata";
constexpr const char *Rsrc = ".rsrc";
constexpr const char *TLS = ".tls";
constexpr const char *Debug = ".debug";
constexpr const char *CRT = ".CRT";
} // namespace coff

namespace macho {
constexpr const char *Text = "__text";
constexpr const char *Data = "__data";
constexpr const char *Bss = "__bss";
constexpr const char *Stubs = "__stubs";
constexpr const char *ObjCStubs = "__objc_stubs";
constexpr const char *StubHelper = "__stub_helper";
constexpr const char *Cstring = "__cstring";
constexpr const char *ObjCMetadataPrefix = "__objc_";
constexpr const char *CfString = "__cfstring";
constexpr const char *La_symbol_ptr = "__la_symbol_ptr";
constexpr const char *Nl_symbol_ptr = "__nl_symbol_ptr";
constexpr const char *Got = "__got";
constexpr const char *Const = "__const";
constexpr const char *Unwind = "__unwind_info";
constexpr const char *EhFrame = "__eh_frame";
constexpr const char *GccExceptTab = "__gcc_except_tab";
constexpr const char *CompactUnwind = "__compact_unwind";
constexpr const char *GoPclnTab = "__gopclntab";
constexpr const char *TextSeg = "__TEXT";
constexpr const char *DataSeg = "__DATA";
constexpr const char *DataConstSeg = "__DATA_CONST";
constexpr const char *LinkeditSeg = "__LINKEDIT";
} // namespace macho

/// Single authoritative test for "is this a primary code/text section?",
/// format-agnostic across COFF/ELF/Mach-O. Prefer this over open-coding name
/// comparisons so every pipeline stage agrees on what counts as text.
///   - Mach-O      : "__text"            (exact)
///   - ELF / COFF  : ".text"             (exact)
///   - ELF split   : ".text.<name>"      (e.g. ".text.hot", ".text.unlikely")
///   - COFF grouped: ".text$<name>"      (e.g. ".text$mn", ".text$x")
/// The mandatory '.' / '$' separator after ".text" deliberately excludes
/// unrelated names such as MSVC's ".textbss".
inline bool isTextSectionName(llvm::StringRef Name) {
  return Name == macho::Text || Name == elf::Text ||
         Name.starts_with(elf::TextSplitPrefix) ||
         Name.starts_with(coff::TextPrefix);
}

/// True if \p Name is a RELRO pointer-table section (`.data.rel.ro` and its
/// `.data.rel.ro.*` variants).  Writable in section flags but read-only after
/// relocation; never plain mutable scalar/array data.
inline bool isDataRelRoSectionName(llvm::StringRef Name) {
  return Name.starts_with(elf::DataRelRo);
}

/// True if \p Name is an ELF data/rodata/bss section whose contents are
/// embedded alongside code in roundtrip and Unicorn test images.
inline bool isElfImageDataSectionName(llvm::StringRef Name) {
  return Name.starts_with(elf::Rodata) || isDataRelRoSectionName(Name) ||
         Name == elf::Data || Name.starts_with(elf::DataSplitPrefix) ||
         Name == elf::Bss || Name.starts_with(elf::BssSplitPrefix);
}

/// True if \p Name is an ELF linker-map output section that carries
/// executable symbols (`.text*`, `.init`, `.fini`, `.plt`, `.plt.got`).
inline bool isELFExecutableMapSection(llvm::StringRef Name) {
  if (Name.starts_with(elf::Text))
    return true;
  return llvm::StringSwitch<bool>(Name)
#define ELF_EXECUTABLE_MAP_SECTION(Section) .Case(Section, true)
#include "neverd/object/ELFExecutableMapSections.inc"
#undef ELF_EXECUTABLE_MAP_SECTION
      .Default(false);
}

/// True if \p Segment/\p Section name a Mach-O linker-map row that carries
/// executable symbols (`__TEXT` + `__text`/`__stubs`).
inline bool isMachOExecutableMapSection(llvm::StringRef Segment,
                                        llvm::StringRef Section) {
  if (Segment != macho::TextSeg)
    return false;
  return llvm::StringSwitch<bool>(Section)
#define MACHO_EXECUTABLE_MAP_SECTION(SectionName) .Case(SectionName, true)
#include "neverd/object/MachOExecutableMapSections.inc"
#undef MACHO_EXECUTABLE_MAP_SECTION
      .Default(false);
}

} // namespace section_names
} // namespace neverd

#endif // NEVERD_OBJECT_SECTIONNAMES_H
