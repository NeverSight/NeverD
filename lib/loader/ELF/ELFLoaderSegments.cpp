//===- ELFLoaderSegments.cpp - ELF segment mapping and layout -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds the address space of a loaded ELF image.  An executable or shared
/// object is laid out from its PT_LOAD program headers; a relocatable object
/// has none, so its SHF_ALLOC sections become the load regions instead, at
/// the bases the caller synthesized for them.
///
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImageFlags.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"

#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {
namespace elf_loader {
namespace detail {

template <typename ELFT>
llvm::Error buildSegments(const llvm::object::ELFFile<ELFT> &ELF,
                          llvm::ArrayRef<typename ELFT::Shdr> Sections,
                          llvm::StringRef ShStrTab, const uint8_t *Data,
                          size_t Size, const std::vector<va_t> &SecBase,
                          bool IsRelocatable, BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Phdr = typename ELFT::Phdr;
  using Elf_Shdr = typename ELFT::Shdr;

  // --- PT_LOAD segments ---
  va_t Lo = InvalidVA;
  auto PhdrsOr = ELF.program_headers();
  if (!PhdrsOr)
    return PhdrsOr.takeError();

  for (const Elf_Phdr &PH : *PhdrsOr) {
    if (PH.p_type != PT_LOAD)
      continue;
    if (PH.p_filesz > PH.p_memsz)
      return llvm::make_error<llvm::StringError>(
          "elf: PT_LOAD file size exceeds memory size",
          llvm::inconvertibleErrorCode());
    if (PH.p_memsz > InvalidVA - static_cast<va_t>(PH.p_vaddr))
      return llvm::make_error<llvm::StringError>(
          "elf: PT_LOAD virtual address range overflows",
          llvm::inconvertibleErrorCode());
    if (PH.p_filesz > 0 && !rangeInBounds(PH.p_offset, PH.p_filesz, Size))
      return llvm::make_error<llvm::StringError>(
          "elf: PT_LOAD file range is out of bounds",
          llvm::inconvertibleErrorCode());

    Segment Seg;
    Seg.Name = (kLoadSegmentPrefix + std::to_string(Img.Segments.size())).str();
    Seg.VA = PH.p_vaddr;
    Seg.Size = PH.p_memsz;
    Seg.FileOff = PH.p_offset;
    Seg.FileSz = PH.p_filesz;
    Seg.Flags = elfPFlagsToNd(PH.p_flags);

    if (PH.p_filesz > 0)
      Seg.Data.assign(Data + PH.p_offset, Data + PH.p_offset + PH.p_filesz);
    // p_memsz is untrusted; only zero-fill up to the cap (see
    // kMaxSegmentZeroFill) so a crafted size cannot force a huge allocation.
    if (PH.p_memsz > PH.p_filesz && PH.p_memsz <= limits::kMaxSegmentZeroFill)
      Seg.Data.resize(static_cast<size_t>(PH.p_memsz), 0);

    if (Seg.VA < Lo)
      Lo = Seg.VA;
    Img.Segments.push_back(std::move(Seg));
  }

  Img.Base = (Lo != InvalidVA) ? Lo : 0;

  // Relocatable objects: use SHF_ALLOC sections as load regions.
  if (Img.Segments.empty()) {
    for (uint32_t I = 0; I < Sections.size(); ++I) {
      const Elf_Shdr &SH = Sections[I];
      if (!(SH.sh_flags & SHF_ALLOC) || SH.sh_size == 0)
        continue;
      Segment Seg;
      llvm::StringRef Name = getSectionName<ELFT>(ShStrTab, SH);
      if (!Name.empty())
        Seg.Name = Name.str();
      Seg.VA = sectionVA<ELFT>(IsRelocatable, SecBase, SH, I);
      Seg.Size = SH.sh_size;
      Seg.FileOff = SH.sh_offset;
      Seg.FileSz = SH.sh_type == SHT_NOBITS ? 0 : SH.sh_size;
      Seg.Flags = elfSHFlagsToNd(SH.sh_flags);
      if (Seg.Size > InvalidVA - Seg.VA)
        return llvm::make_error<llvm::StringError>(
            "elf: allocatable section range overflows",
            llvm::inconvertibleErrorCode());
      if (Seg.FileSz > 0 && !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
        return llvm::make_error<llvm::StringError>(
            "elf: allocatable section file range is out of bounds",
            llvm::inconvertibleErrorCode());
      if (Seg.FileSz > 0)
        Seg.Data.assign(Data + SH.sh_offset, Data + SH.sh_offset + SH.sh_size);
      else if (Seg.Size <= limits::kMaxSegmentZeroFill)
        Seg.Data.resize(static_cast<size_t>(Seg.Size), 0);
      Img.Segments.push_back(std::move(Seg));
    }
    if (!Img.Segments.empty()) {
      Lo = InvalidVA;
      for (const auto &S : Img.Segments)
        if (S.VA < Lo)
          Lo = S.VA;
      Img.Base = (Lo != InvalidVA) ? Lo : 0;
    }
  }

  if (Img.Segments.empty())
    return llvm::make_error<llvm::StringError>(
        "elf: no loadable segments found", llvm::inconvertibleErrorCode());

  // Overlay section names onto segments.
  for (const Elf_Shdr &SH : Sections) {
    if (SH.sh_addr == 0 || SH.sh_size == 0)
      continue;
    llvm::StringRef SectName = getSectionName<ELFT>(ShStrTab, SH);
    if (SectName.empty())
      continue;
    for (auto &Seg : Img.Segments) {
      if (Seg.contains(SH.sh_addr)) {
        Seg.Name = SectName.str();
        break;
      }
    }
  }

  return llvm::Error::success();
}

// ===--------------------------------------------------------------------===//
// Explicit template instantiations for ELF32 and ELF64
// ===--------------------------------------------------------------------===//

template llvm::Error buildSegments<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, llvm::StringRef,
    const uint8_t *, size_t, const std::vector<va_t> &, bool, BinaryImage &);
template llvm::Error buildSegments<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, llvm::StringRef,
    const uint8_t *, size_t, const std::vector<va_t> &, bool, BinaryImage &);

} // namespace detail
} // namespace elf_loader
} // namespace neverd
