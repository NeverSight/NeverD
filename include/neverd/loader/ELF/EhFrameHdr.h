//===- EhFrameHdr.h - .eh_frame_hdr function discovery ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses the ELF .eh_frame_hdr section to discover function entry
/// points from the FDE binary-search table.  Encoding constants follow
/// the DWARF EH pointer format (llvm/BinaryFormat/Dwarf.h).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_EHFRAMEHDR_H
#define NEVERD_LOADER_ELF_EHFRAMEHDR_H

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {
namespace elf_loader {

// LLVM_DEBUG expands DEBUG_TYPE where it appears, but this template lives in a
// header the ELF loader includes before defining its own DEBUG_TYPE; define a
// header-local one here and undefine it after so the header is self-contained
// and the macro never leaks to the including TU.
#define DEBUG_TYPE "neverd-elf-loader"

/// Add function symbols from .eh_frame_hdr if present.
/// \p SectionName must be ".eh_frame_hdr".
template <typename ShdrT>
void addFunctionsFromEhFrameHdr(const uint8_t *Data, size_t FileSize,
                                const ShdrT &SH, BinaryImage &Img) {
  using namespace dweh;

  size_t SecSize = static_cast<size_t>(SH.sh_size);
  // sh_offset/sh_size are untrusted section-header fields; use rangeInBounds so
  // a crafted offset near the integer maximum cannot wrap this check into an
  // out-of-bounds read at Base below.
  if (!rangeInBounds(SH.sh_offset, SecSize, FileSize) ||
      SecSize < kEhFrameHdrMinSize)
    return;

  const uint8_t *Base = Data + SH.sh_offset;
  auto HdrFields = *reinterpret_cast<const EhFrameHdrHeader *>(Base);

  if (HdrFields.Version != kEhFrameHdrVersion)
    return;

  uint8_t TableApp = getApplication(HdrFields.TableEnc);
  bool IsDataRel = (TableApp == DataRel);
  bool IsPCRel = (TableApp == PCRel);

  if (getFormat(HdrFields.TableEnc) != Sdata4)
    return;

  size_t Cursor = sizeof(EhFrameHdrHeader);

  size_t PtrSize = getEncodedSize(HdrFields.EhFramePtrEnc);
  if (PtrSize == 0)
    return;
  Cursor += PtrSize;

  size_t CntSize = getEncodedSize(HdrFields.FdeCountEnc);
  if (CntSize == 0 || !rangeInBounds(Cursor, CntSize, SecSize))
    return;
  uint32_t FdeCount = static_cast<uint32_t>(
      readEncoded(Base, SecSize, Cursor, HdrFields.FdeCountEnc));

  // FdeCount is an untrusted count; form the table size in 64 bits and use
  // rangeInBounds so the multiply cannot wrap the check into an out-of-bounds
  // FDE read in the loop below.
  if (FdeCount == 0 ||
      !rangeInBounds(Cursor, static_cast<uint64_t>(FdeCount) * kFdeEntrySize,
                     SecSize))
    return;

  auto Existing = Img.getSymbolAddresses();

  [[maybe_unused]] size_t Added = 0;
  for (uint32_t F = 0; F < FdeCount; ++F) {
    int32_t InitLoc;
    std::memcpy(&InitLoc, Base + Cursor + F * kFdeEntrySize, sizeof(InitLoc));

    va_t FuncAddr;
    if (IsDataRel)
      FuncAddr = SH.sh_addr + static_cast<int64_t>(InitLoc);
    else if (IsPCRel)
      FuncAddr = SH.sh_addr + Cursor + F * kFdeEntrySize +
                 static_cast<int64_t>(InitLoc);
    else
      FuncAddr = static_cast<va_t>(static_cast<int64_t>(InitLoc));

    if (Img.Arch == Arch::ARM)
      FuncAddr = clearThumbBit(FuncAddr);

    if (FuncAddr == 0 || !Existing.insert(FuncAddr).second)
      continue;

    Img.Symbols.push_back(Symbol::makeFunc(FuncAddr));
    ++Added;
  }
  LLVM_DEBUG(llvm::dbgs() << "elf: .eh_frame_hdr added " << Added
                          << " functions from " << FdeCount << " FDEs\n");
}

#undef DEBUG_TYPE

} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LOADER_ELF_EHFRAMEHDR_H
