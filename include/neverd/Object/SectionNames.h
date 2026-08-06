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

namespace neverd {
namespace section_names {

namespace elf {
constexpr const char *Text = ".text";
// Function/section split produced by -ffunction-sections and hot/cold
// splitting: ".text.hot", ".text.unlikely", ".text.<symbol>", ...
constexpr const char *TextSplitPrefix = ".text.";
constexpr const char *Data = ".data";
constexpr const char *Bss = ".bss";
constexpr const char *Rodata = ".rodata";
constexpr const char *RelaPlt = ".rela.plt";
constexpr const char *RelPlt = ".rel.plt";
constexpr const char *Plt = ".plt";
constexpr const char *PltPrefix = ".plt.";
constexpr const char *PltGot = ".plt.got";
constexpr const char *Iplt = ".iplt";
constexpr const char *EhFrameHdr = ".eh_frame_hdr";
constexpr const char *EhFrame = ".eh_frame";
constexpr const char *Dynamic = ".dynamic";
constexpr const char *Got = ".got";
constexpr const char *GotPlt = ".got.plt";
constexpr const char *Dynsym = ".dynsym";
constexpr const char *Dynstr = ".dynstr";
constexpr const char *Init = ".init";
constexpr const char *Fini = ".fini";
constexpr const char *InitArray = ".init_array";
constexpr const char *FiniArray = ".fini_array";
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
constexpr const char *StubHelper = "__stub_helper";
constexpr const char *Cstring = "__cstring";
constexpr const char *La_symbol_ptr = "__la_symbol_ptr";
constexpr const char *Nl_symbol_ptr = "__nl_symbol_ptr";
constexpr const char *Got = "__got";
constexpr const char *Const = "__const";
constexpr const char *Unwind = "__unwind_info";
constexpr const char *EhFrame = "__eh_frame";
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

} // namespace section_names
} // namespace neverd

#endif // NEVERD_OBJECT_SECTIONNAMES_H
