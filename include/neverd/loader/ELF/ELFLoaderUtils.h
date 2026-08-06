//===- ELFLoaderUtils.h - ELF loader helpers ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF-specific loader utilities: .dynamic parsing, .rela.plt/.rel.plt
/// import resolution, .eh_frame_hdr function discovery, and PLT stub
/// scanning.  Mirrors the COFF/MachO *LoaderUtils.h pattern for
/// structural symmetry.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_ELFLOADERUTILS_H
#define NEVERD_LOADER_ELF_ELFLOADERUTILS_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Object/ELFObjectFile.h"

#include <optional>

namespace neverd {
namespace elf_loader {

/// Parse the .dynamic section and populate DynInfo (NEEDED, SONAME, RPATH,
/// INIT/FINI).
template <typename ELFT>
void parseDynamic(const llvm::object::ELFFile<ELFT> &ELF,
                  const typename ELFT::Shdr &DynamicSH, const uint8_t *Data,
                  size_t Size, BinaryImage &Img);

/// Read an ELF function-pointer array from the mapped preferred image,
/// retaining only executable targets and deduplicating \p Out.
void recordRuntimePointerArray(va_t Addr, uint64_t Size, std::vector<va_t> &Out,
                               BinaryImage &Img);

/// Parse section-backed preinit/init/fini arrays, including the legacy
/// .ctors/.dtors spellings that are not necessarily described by PT_DYNAMIC.
void parseRuntimeSections(BinaryImage &Img);

/// Record the resolver named by an architecture-specific GNU IRELATIVE
/// relocation.  RELA supplies \p Addend; REL reads the implicit addend from
/// the mapped relocation slot.
bool recordIRelativeResolver(uint32_t RelocType, va_t Slot,
                             std::optional<int64_t> Addend, BinaryImage &Img);

/// Parse .rela.plt / .rel.plt entries and populate Img.Imports with
/// PLT-resolved external symbols.
template <typename ELFT>
void parsePLTImports(const llvm::object::ELFFile<ELFT> &ELF,
                     llvm::ArrayRef<typename ELFT::Shdr> Sections,
                     const uint8_t *Data, size_t Size, BinaryImage &Img);

/// Add function symbols from the .eh_frame_hdr section's binary-search
/// table.  Template \p ShdrT must match the ELF class (Elf32_Shdr /
/// Elf64_Shdr).  Defined in EhFrameHdr.h.
template <typename ShdrT>
void addFunctionsFromEhFrameHdr(const uint8_t *Data, size_t FileSize,
                                const ShdrT &SH, BinaryImage &Img);

/// Parse .got and .got.plt sections and record GOT entry addresses
/// into the image's relocation table for cross-reference analysis.
template <typename ELFT>
void parseGOTEntries(const llvm::object::ELFFile<ELFT> &ELF,
                     llvm::ArrayRef<typename ELFT::Shdr> Sections,
                     const uint8_t *Data, size_t Size, BinaryImage &Img);

/// Parse ELF PT_NOTE segments or SHT_NOTE sections.
/// Extracts build-id, ABI tag, and other note descriptors.
template <typename ELFT>
void parseNotes(const llvm::object::ELFFile<ELFT> &ELF, const uint8_t *Data,
                size_t Size, BinaryImage &Img);

} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LOADER_ELF_ELFLOADERUTILS_H
