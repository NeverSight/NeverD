//===- SourceParameterPlacementMIPS.cpp - MIPS scalar carriers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MIPS o32 FP64, n32 and n64 scalar hard-float parameter locations.
/// ELF source admission authenticates the FP profile; records are not
/// classified.
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::source_placement;

std::optional<SourceParameterPlacement> source_placement::placeMIPS(
    const TargetRegInfo &TRI, BinaryFormat F, const TypeRef &ReturnType,
    llvm::ArrayRef<TypeRef> ParamTypes, std::optional<unsigned> FixedCount) {
  if (F != BinaryFormat::ELF || !ReturnType ||
      (ReturnType->Kind != NdTypeKind::Void &&
       ((!isIntegerScalar(ReturnType) && !isFloatingScalar(ReturnType)) ||
        ReturnType->Size >
            (ReturnType->Kind == NdTypeKind::Int ? 2 * TRI.FullRegWidth : 8))))
    return std::nullopt;
  const auto Layout = TRI.integerArgumentLayout(F);
  const bool Little = archLittleEndian(TRI.TheArch);
  const unsigned Slot = Layout.SlotBytes;
  unsigned Next = 0;
  unsigned FloatingPrefix = 0;
  bool LeadingFloating = !FixedCount;
  SourceParameterPlacement Result;
  for (const TypeRef &Type : ParamTypes) {
    const bool Floating = isFloatingScalar(Type);
    if (Floating && Type->Size != 4 && Type->Size != 8)
      break;
    if ((!isIntegerScalar(Type) && !Floating) ||
        (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
         Type->Size != 8))
      break;
    // o32 long long uses an even/odd register pair, or an 8-byte aligned
    // stack pair; it never straddles a3 and the first stack argument.
    if (Slot == 4 && Type->Size == 8)
      Next = (Next + 1) & ~1u;
    const unsigned Count = (Type->Size + Slot - 1) / Slot;
    const bool Named = !FixedCount || Result.Parameters.size() < *FixedCount;
    auto &Pieces = Result.Parameters.emplace_back();
    // n32 and n64 share eight argument positions between a0-a7 and f12-f19.
    // o32 uses f12/f14 only for the first two leading FP arguments; their
    // word positions still consume the integer argument area.
    const bool InFPR =
        Floating &&
        (Slot == 8 ? Named && Next < 8 : LeadingFloating && FloatingPrefix < 2);
    LeadingFloating &= Floating;
    if (InFPR) {
      const unsigned Register =
          Slot == 8 ? 12 + Next : 12 + 2 * FloatingPrefix++;
      Pieces.push_back(registerPiece(SourceABICarrierKind::FloatingRegister,
                                     mipsreg::w(Register), 0, Type->Size));
      Next += Count;
      continue;
    }
    for (unsigned I = 0; I < Count; ++I, ++Next) {
      const auto Bytes =
          static_cast<uint16_t>(std::min<unsigned>(Type->Size, Slot));
      // Integer pieces use significance offsets, not host byte addresses.
      const auto Offset =
          static_cast<uint16_t>((Little ? I : Count - I - 1) * Slot);
      if (Next < Layout.Registers.size()) {
        Pieces.push_back(registerPiece(SourceABICarrierKind::IntegerRegister,
                                       Layout.Registers[Next], Offset, Bytes));
      } else {
        const int64_t Stack =
            Layout.EntryStackBase + (Next - Layout.Registers.size()) * Slot;
        // Big-endian integers are right-justified; floating values occupy
        // the lowest addressed bytes of the stack slot.
        Pieces.push_back(stackPiece(
            Stack + (Little || Floating ? 0 : Slot - Bytes), Offset, Bytes));
      }
    }
  }
  return Result;
}
