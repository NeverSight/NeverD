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
#include "neverd/loader/ObjectExterns.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

/// Read a linked string table from either original or reconstructed headers.
/// LLVM's string-table error diagnostics require an original section index;
/// reconstructed headers have none. Invalid names do not discard numeric
/// symbol values or the relocations that use them.
template <typename ELFT>
inline llvm::StringRef
getLinkedStringTable(const llvm::object::ELFFile<ELFT> &ELF,
                     llvm::ArrayRef<typename ELFT::Shdr> Sections,
                     const typename ELFT::Shdr &SH) {
  const auto *Strings = getShdr<ELFT>(Sections, SH.sh_link);
  if (!Strings || Strings->sh_type != llvm::ELF::SHT_STRTAB ||
      Strings->sh_size == 0 ||
      !rangeInBounds(Strings->sh_offset, Strings->sh_size, ELF.getBufSize()))
    return {};
  const llvm::StringRef Text(
      reinterpret_cast<const char *>(ELF.base() + Strings->sh_offset),
      Strings->sh_size);
  return Text.back() == '\0' ? Text : llvm::StringRef{};
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

/// The addresses a relocatable object's relocations resolve against that no
/// section of the object holds (ELFLoaderExterns.cpp): its externs, laid out
/// by the shared object-extern layer, and the GOT entry the linker would
/// create for each symbol a GOT reference names, by (symbol table section,
/// symbol index), which holds the symbol's address.  The cells are the GOT.
struct ObjectExterns : object_externs::ExternLayout {
  std::map<std::pair<uint32_t, uint32_t>, va_t> GOTEntries;
};

/// The relocation a GOT reference \p Type of \p A applies against the GOT
/// entry of its symbol (ELFObjectRelocations.def), or nullopt.
std::optional<uint32_t> directTypeOfGOTReference(Arch A, uint32_t Type);

/// True when \p Type relocates a call or branch of \p A, so the symbol it
/// names is a function (ELFObjectRelocations.def).
bool isBranchReference(Arch A, uint32_t Type);

/// Place the undefined and common symbols of a relocatable object, and the
/// GOT entries its GOT references reach, past \p ImageEnd, the end of its
/// allocated sections.
template <typename ELFT>
llvm::Expected<ObjectExterns>
planObjectExterns(const llvm::object::ELFFile<ELFT> &ELF,
                  llvm::ArrayRef<typename ELFT::Shdr> Sections,
                  const uint8_t *Data, size_t Size, Arch A, va_t ImageEnd);

/// Add the planned extern segment and GOT to \p Img with their sections, the
/// undefined symbols as symbols and the called ones as imports, and the GOT
/// entries holding their symbols' addresses.
template <typename ELFT>
void addObjectExterns(const llvm::object::ELFFile<ELFT> &ELF,
                      llvm::ArrayRef<typename ELFT::Shdr> Sections,
                      const std::vector<va_t> &SecBase,
                      const ObjectExterns &Externs, BinaryImage &Img);

/// Apply the relocations of a relocatable object in place, patching the
/// segment bytes the lifter reads and recording the address-taken targets and
/// pointer slots the emitter needs in order to symbolize them again.  An
/// undefined or common symbol resolves to its address in \p Externs, and a GOT
/// reference to its symbol's GOT entry.
template <typename ELFT>
llvm::Error applyRelocations(const llvm::object::ELFFile<ELFT> &ELF,
                             llvm::ArrayRef<typename ELFT::Shdr> Sections,
                             const uint8_t *Data, size_t Size,
                             const std::vector<va_t> &SecBase,
                             bool IsRelocatable, const ObjectExterns &Externs,
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
/// A common symbol takes its address in \p CommonSlots.  A symbol's
/// `st_shndx` indexes \p OwnerSections, the file's own section headers, which
/// are \p Sections unless those were reconstructed.
template <typename ELFT>
llvm::Error collectSymbols(const llvm::object::ELFFile<ELFT> &ELF,
                           llvm::ArrayRef<typename ELFT::Shdr> Sections,
                           llvm::ArrayRef<typename ELFT::Shdr> OwnerSections,
                           size_t Size, const std::vector<va_t> &SecBase,
                           bool IsRelocatable,
                           const std::map<std::string, va_t> &CommonSlots,
                           BinaryImage &Img);

/// Section headers for the tables a linked image without section headers
/// still has, and the string table naming them.
template <typename ELFT> struct DynamicSections {
  std::vector<typename ELFT::Shdr> Headers;
  std::string Names;
};

/// Rebuild, for a linked image whose section header table was stripped
/// (`sstrip`, `llvm-objcopy --strip-sections`), the headers of the tables its
/// runtime still reads through the program headers: .dynamic, .dynstr,
/// .dynsym, the dynamic, PLT and RELR relocation tables, and .eh_frame_hdr.
/// They feed the loader's parsers only.  A symbol's `st_shndx` still names a
/// section of the original table, which is gone.  A table the runtime would
/// not find whole is reported in \p Img's load diagnostics and left out.
template <typename ELFT>
DynamicSections<ELFT>
reconstructDynamicSections(const llvm::object::ELFFile<ELFT> &ELF,
                           BinaryImage &Img);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_ELF_ELFLOADERDETAIL_H
