//===- ELFPPC64Entries.cpp - ELFv2 local and global entries ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read the ELFv2 symbol's local-entry contract and the TOC established by
/// the global-entry instructions. Different functions can use different TOCs.
///
//===----------------------------------------------------------------------===//

#include "ELFIntegerProfile.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::elf_loader {
namespace {
using namespace ppc_elf;

std::optional<va_t> entryTOC(const BinaryImage &Image, va_t Entry,
                             unsigned Offset) {
  const uint8_t *Code = Image.readVA(Entry, Offset);
  if (!Code || !Offset)
    return std::nullopt;
  const bool Little = archLittleEndian(Image.Arch);
  std::optional<va_t> R2;
  bool Relative = false;
  for (unsigned I = 0; I < Offset; I += InstructionAlignment) {
    uint32_t Word = readTargetEndian<uint32_t>(Code + I, Little);
    int64_t Imm = llvm::SignExtend64<ImmediateBits>(Word);
    if (Word == Nop)
      continue;
    if ((Word & DSFormMask) == LoadR2R12) {
      // The large code model puts the TOC difference in a literal before
      // the global entry. It is invariant under rebasing.
      const va_t Slot = Entry + Imm;
      const auto *Bytes = Image.readVA(Slot, WordSize);
      const auto *Seg = Image.getSegmentFor(Slot);
      if (!Bytes || !Seg || Seg->isWritable())
        return std::nullopt;
      R2 = readTargetEndian<uint64_t>(Bytes, Little);
      Relative = false;
      continue;
    }
    if ((Word == AddR2R2R12 || Word == AddR2R12R2) && R2 && !Relative) {
      Relative = true;
      *R2 += Entry;
      continue;
    }
    switch (Word & DFormMask) {
    case LisR2:
    case LiR2:
      if (!Image.LoadsAtLinkAddress)
        return std::nullopt;
      R2 = 0;
      Relative = false;
      break;
    case AddisR2R12:
    case AddiR2R12:
      R2 = Entry;
      Relative = true;
      break;
    case AddisR2R2:
    case AddiR2R2:
      if (R2)
        break;
      [[fallthrough]];
    default:
      return std::nullopt;
    }
    if ((Word & DFormMask) == AddisR2R12 || (Word & DFormMask) == AddisR2R2 ||
        (Word & DFormMask) == LisR2)
      Imm *= int64_t(1) << ImmediateBits;
    *R2 += Imm;
  }
  return Relative || Image.LoadsAtLinkAddress ? R2 : std::nullopt;
}
} // namespace

llvm::Error recordPPC64ELFv2Entry(BinaryImage &Image, va_t Entry, uint64_t Size,
                                  uint8_t Other) {
  using namespace llvm::ELF;
  const unsigned Encoding =
      (Other & STO_PPC64_LOCAL_MASK) >> STO_PPC64_LOCAL_BIT;
  const unsigned Offset = decodePPC64LocalEntryOffset(Other);
  if (Encoding == ppc_elf::ReservedLocalEntryEncoding ||
      Entry % ppc_elf::InstructionAlignment ||
      (Size && Size < Offset + ppc_elf::InstructionAlignment) ||
      !Image.isCodeRange(Entry, Offset + ppc_elf::InstructionAlignment))
    return llvm::createStringError("elf: invalid PPC64 ELFv2 local entry");
  auto [It, New] = Image.PPC64EntryContexts.try_emplace(Entry);
  auto &Ctx = It->second;
  if (New) {
    Ctx.LocalEntryOffset = Offset;
    Ctx.PreservesTOC = Encoding == 0;
    Ctx.TOC = entryTOC(Image, Entry, Offset);
  } else if (Ctx.LocalEntryOffset != Offset ||
             Ctx.PreservesTOC != (Encoding == 0))
    return llvm::createStringError(
        "elf: conflicting PPC64 ELFv2 entry aliases");
  return llvm::Error::success();
}
} // namespace neverd::elf_loader
