//===- SourceParameterPlacementMIPS.cpp - o32/n64 integer carriers ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classic MIPS integer and pointer parameters. Floating point and records
/// have separate ABI rules and are deliberately not classified here.
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "neverd/ArchSupport.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::source_placement;

std::optional<SourceParameterPlacement>
source_placement::placeMIPS(const TargetRegInfo &TRI, BinaryFormat F,
                            const TypeRef &ReturnType,
                            llvm::ArrayRef<TypeRef> ParamTypes) {
  if (F != BinaryFormat::ELF || !ReturnType ||
      (ReturnType->Kind != NdTypeKind::Void &&
       (!isIntegerScalar(ReturnType) || ReturnType->Size > 8)))
    return std::nullopt;
  const auto Layout = TRI.integerArgumentLayout(F);
  const bool Little = archLittleEndian(TRI.TheArch);
  const unsigned Slot = Layout.SlotBytes;
  unsigned Next = 0;
  SourceParameterPlacement Result;
  for (const TypeRef &Type : ParamTypes) {
    if (!isIntegerScalar(Type) || (Type->Size != 1 && Type->Size != 2 &&
                                   Type->Size != 4 && Type->Size != 8))
      break;
    // o32 long long uses an even/odd register pair, or an 8-byte aligned
    // stack pair; it never straddles a3 and the first stack argument.
    if (Slot == 4 && Type->Size == 8)
      Next = (Next + 1) & ~1u;
    const unsigned Count = (Type->Size + Slot - 1) / Slot;
    auto &Pieces = Result.Parameters.emplace_back();
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
        // Both ABIs right-justify narrow scalars in big-endian stack slots.
        Pieces.push_back(
            stackPiece(Stack + (Little ? 0 : Slot - Bytes), Offset, Bytes));
      }
    }
  }
  return Result;
}
