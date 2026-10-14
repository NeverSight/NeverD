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

#include <map>
#include <optional>

namespace neverd {
namespace elf_loader {

/// Exact width of a relocation field at r_offset, including a zero-width NONE
/// record. Unknown or biased writers return none. This describes the bytes,
/// not admission of the relocation's semantics by a native object installer.
std::optional<size_t> objectRelocationFieldWidth(Arch Target, uint32_t Type);

/// ABI mapping markers are local, untyped symbols, with an optional suffix.
inline bool isMappingSymbolName(llvm::StringRef Name) {
  return Name.size() >= 2 && Name[0] == '$' &&
         (Name[1] == 'a' || Name[1] == 'd' || Name[1] == 't' ||
          Name[1] == 'x') &&
         (Name.size() == 2 || Name[2] == '.');
}

/// Parse the .dynamic section and populate DynInfo (NEEDED, SONAME, RPATH,
/// INIT/FINI).  Its string table is the one of \p Sections its sh_link names.
template <typename ELFT>
void parseDynamic(const llvm::object::ELFFile<ELFT> &ELF,
                  llvm::ArrayRef<typename ELFT::Shdr> Sections,
                  const typename ELFT::Shdr &DynamicSH, const uint8_t *Data,
                  size_t Size, BinaryImage &Img);

/// Read an ELF function-pointer array from the mapped preferred image,
/// retaining only executable targets and deduplicating \p Out.
void recordRuntimePointerArray(va_t Addr, uint64_t Size, std::vector<va_t> &Out,
                               BinaryImage &Img);

/// Parse section-backed preinit/init/fini arrays, including the legacy
/// .ctors/.dtors spellings that are not necessarily described by PT_DYNAMIC.
void parseRuntimeSections(BinaryImage &Img);

/// Whether ELF relocation \p RelocType stores a symbol's address in a GOT
/// slot the dynamic linker binds (GLOB_DAT, JUMP_SLOT).
bool isELFSlotBinding(Arch Target, uint32_t RelocType);

/// Record the slot of a dynamic relocation that binds an undefined symbol's
/// address as import storage, as the Mach-O loader records its binds: the
/// slot holds the external symbol, so a call through it calls the import.
/// \p Undefined says whether the symbol is undefined in this image.
bool recordImportSlotBinding(uint32_t RelocType, va_t Slot,
                             llvm::StringRef Symbol, bool Undefined,
                             int64_t Addend, BinaryImage &Img);

/// Record the resolver named by an architecture-specific GNU IRELATIVE
/// relocation.  RELA supplies \p Addend; REL reads the implicit addend from
/// the mapped relocation slot.
bool recordIRelativeResolver(uint32_t RelocType, va_t Slot,
                             std::optional<int64_t> Addend, BinaryImage &Img);

/// Record the slot of a dynamic relocation the dynamic linker fills with a
/// function only it chooses -- a GNU indirect function's implementation
/// (IRELATIVE) or a TLS descriptor's resolver: a call through it calls what
/// the slot holds when the call runs.
bool recordRuntimeCallableRelocation(uint32_t RelocType, va_t Slot,
                                     BinaryImage &Img);

/// What the dynamic linker writes for relocation \p RelocType bound to a
/// symbol at \p SymbolVA with \p Addend; none for a type that does not store
/// its symbol's address.
std::optional<uint64_t> symbolRelocationValue(Arch Target, uint32_t RelocType,
                                              uint64_t SymbolVA,
                                              int64_t Addend);

/// Parse .rela.plt / .rel.plt entries and populate Img.Imports with
/// PLT-resolved external symbols.  \p ShStrTab names \p Sections.
template <typename ELFT>
void parsePLTImports(const llvm::object::ELFFile<ELFT> &ELF,
                     llvm::ArrayRef<typename ELFT::Shdr> Sections,
                     llvm::StringRef ShStrTab, const uint8_t *Data, size_t Size,
                     BinaryImage &Img);

/// Attach each ARM `.plt` veneer to the import it forwards to.
///
/// `parsePLTImports` names an import by the GOT cell the dynamic linker binds,
/// which is the address every *data* reference to it uses.  Code references do
/// not go there: they branch to the veneer, and on ARM so does the address a
/// generic `.ARM.extab` entry names for its personality routine.  Without the
/// pairing every such routine reads as unnamed, and the frames that install it
/// lose the one fact that says what their landing pads do.
///
/// A veneer is paired only when its own instructions compute a GOT cell an
/// import already claimed, so nothing is attached on the strength of position
/// in the table -- the layout a lazily bound PLT happens to have is not the
/// one `-z now`, `.plt.sec`, or an IFUNC produces.
size_t recordARMPLTVeneers(BinaryImage &Img);

/// The ARM `.plt` veneers of \p Img and the imports they forward to, found
/// as \ref recordARMPLTVeneers finds them, without recording anything.
std::map<va_t, size_t> findARMPLTVeneers(const BinaryImage &Img);

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
