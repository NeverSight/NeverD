//===- ELFLoaderDetail.h - Private ELF image construction -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the ELF loader in `lib/loader/ELF`.  The phases
/// declared here are the successive steps `loadELF` drives -- address-space
/// layout, the section and symbol tables, and the relocation tables -- split
/// across translation units and instantiated for ELF32LE and ELF64LE only.
/// Nothing outside this directory may include this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_ELF_ELFLOADERDETAIL_H
#define NEVERD_LIB_LOADER_ELF_ELFLOADERDETAIL_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd {
namespace elf_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

/// Name of \p SH, read out of the section header string table \p ShStrTab.
/// Empty when the header's `sh_name` does not index into that table.
template <typename ELFT>
inline llvm::StringRef getSectionName(llvm::StringRef ShStrTab,
                                      const typename ELFT::Shdr &SH) {
  if (SH.sh_name >= ShStrTab.size())
    return {};
  return ShStrTab.substr(SH.sh_name).split('\0').first;
}

/// Section header \p Idx of \p Sections, or null when the index is out of
/// range.  Section header fields naming another section are untrusted.
template <typename ELFT>
inline const typename ELFT::Shdr *
getShdr(llvm::ArrayRef<typename ELFT::Shdr> Sections, uint32_t Idx) {
  if (Idx >= Sections.size())
    return nullptr;
  return &Sections[Idx];
}

/// Address section \p Idx is loaded at.  A relocatable object carries no
/// addresses of its own, so it uses the base synthesized for the section;
/// everything else uses `sh_addr`, falling back to the file offset.
template <typename ELFT>
inline va_t sectionVA(bool IsRelocatable, const std::vector<va_t> &SecBase,
                      const typename ELFT::Shdr &SH, uint32_t Idx) {
  if (IsRelocatable)
    return SecBase[Idx];
  return SH.sh_addr ? static_cast<va_t>(SH.sh_addr)
                    : static_cast<va_t>(SH.sh_offset);
}

/// Build the image's address space from the PT_LOAD program headers, falling
/// back to the SHF_ALLOC sections for a relocatable object that has none, and
/// name each resulting segment after the section it holds.  Sets `Img.Base`.
template <typename ELFT>
llvm::Error buildSegments(const llvm::object::ELFFile<ELFT> &ELF,
                          llvm::ArrayRef<typename ELFT::Shdr> Sections,
                          llvm::StringRef ShStrTab, const uint8_t *Data,
                          size_t Size, const std::vector<va_t> &SecBase,
                          bool IsRelocatable, BinaryImage &Img);

/// Record every section header as an `Img.Sections` entry, together with the
/// bytes it is backed by.
template <typename ELFT>
llvm::Error buildSections(llvm::ArrayRef<typename ELFT::Shdr> Sections,
                          llvm::StringRef ShStrTab, const uint8_t *Data,
                          size_t Size, const std::vector<va_t> &SecBase,
                          bool IsRelocatable, BinaryImage &Img);

/// Record every SHT_REL / SHT_RELA entry in `Img.Relocations`, resolving each
/// one's symbol name where the referenced symbol table supplies it.
template <typename ELFT>
void collectRelocations(const llvm::object::ELFFile<ELFT> &ELF,
                        llvm::ArrayRef<typename ELFT::Shdr> Sections,
                        llvm::StringRef ShStrTab, const uint8_t *Data,
                        size_t Size, bool IsRelocatable, BinaryImage &Img);

/// Apply the relocations of a relocatable object in place, patching the
/// segment bytes the lifter reads and recording the address-taken targets and
/// pointer slots the emitter needs in order to symbolize them again.
template <typename ELFT>
void applyRelocations(const llvm::object::ELFFile<ELFT> &ELF,
                      llvm::ArrayRef<typename ELFT::Shdr> Sections,
                      const uint8_t *Data, size_t Size,
                      const std::vector<va_t> &SecBase, bool IsRelocatable,
                      BinaryImage &Img);

/// Apply full-width dynamic-loader-relative relocations in a linked ELF image
/// at its link-time virtual addresses, and normalize their pointer provenance
/// into the same slot/target sets used by relocatable objects and other
/// container formats.
template <typename ELFT>
void applyDynamicRelativeRelocations(
    const llvm::object::ELFFile<ELFT> &ELF,
    llvm::ArrayRef<typename ELFT::Shdr> Sections, const uint8_t *Data,
    size_t Size, BinaryImage &Img);

/// Record the defined symbols of every SHT_SYMTAB / SHT_DYNSYM section in
/// `Img.Symbols`, and every global or weak function among them in
/// `Img.Exports`.
template <typename ELFT>
void collectSymbols(const llvm::object::ELFFile<ELFT> &ELF,
                    llvm::ArrayRef<typename ELFT::Shdr> Sections, size_t Size,
                    const std::vector<va_t> &SecBase, bool IsRelocatable,
                    BinaryImage &Img);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_ELF_ELFLOADERDETAIL_H
