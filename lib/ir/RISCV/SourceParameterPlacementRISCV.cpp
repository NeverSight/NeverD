//===- SourceParameterPlacementRISCV.cpp - RISC-V ELF psABI ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>

using namespace neverd;
using namespace neverd::source_placement;

namespace {
using FloatingPieces = llvm::SmallVector<NdScalarLeaf, 2>;

bool fitsFloatingRegister(unsigned Size) {
  return Size == 2 || Size == 4 || Size == 8;
}

// A present empty list uses the integer convention. A missing list leaves the
// declaration unresolved, for example when a record has no known layout.
std::optional<FloatingPieces> floatingPieces(const TypeRef &Type,
                                             unsigned Word) {
  if (!Type || !Type->Size)
    return std::nullopt;
  FloatingPieces Pieces;
  if (isFloatingScalar(Type) && fitsFloatingRegister(Type->Size))
    Pieces.push_back({0, Type->Size, true});
  else if (isRecord(Type)) {
    if (Type->Passing == NdRecordPassing::ByReference)
      return Pieces;
    if (Type->Passing != NdRecordPassing::ByValue &&
        Type->Passing != NdRecordPassing::Complex)
      return std::nullopt;
    if (Type->IsUnion)
      return Pieces;
    bool ContainsUnion = false;
    auto Leaves = scalarLeaves(Type, &ContainsUnion);
    if (ContainsUnion)
      return Pieces;
    if (!Leaves)
      return std::nullopt;
    // Fields of a structure must lie within its extent and cannot overlap.
    if (llvm::any_of(*Leaves,
                     [&](auto L) { return L.Offset + L.Size > Type->Size; }) ||
        (Leaves->size() == 2 &&
         (*Leaves)[0].Offset + (*Leaves)[0].Size > (*Leaves)[1].Offset))
      return std::nullopt;
    if (Leaves->size() <= 2 &&
        llvm::any_of(*Leaves, [](auto L) { return L.Floating; }) &&
        llvm::all_of(*Leaves, [Word](auto L) {
          return L.Size &&
                 (L.Floating ? fitsFloatingRegister(L.Size) : L.Size <= Word);
        }))
      return FloatingPieces(Leaves->begin(), Leaves->end());
  } else if (!isIntegerScalar(Type) && !isFloatingScalar(Type))
    return std::nullopt;
  return Pieces;
}
} // namespace

std::optional<SourceParameterPlacement> source_placement::placeRISCVDoubleABI(
    const TargetRegInfo &TRI, const TypeRef &ReturnType,
    llvm::ArrayRef<TypeRef> Types, unsigned FixedCount) {
  if (!ReturnType)
    return std::nullopt;
  const unsigned Word = TRI.PointerSize;
  using K = SourceABICarrierKind;
  unsigned GP = 0, FP = 0;
  uint64_t Stack = 0;
  SourceParameterPlacement Result;
  if (ReturnType->Kind != NdTypeKind::Void) {
    const auto Parts = floatingPieces(ReturnType, Word);
    if (!Parts)
      return std::nullopt;
    if (ReturnType->Passing == NdRecordPassing::ByReference ||
        (Parts->empty() && ReturnType->Size > 2 * Word))
      Result.ResultPointer =
          registerPiece(K::IntegerRegister, TRI.IntParamRegs[GP++], 0, Word)
              .Location;
  }

  for (auto [Index, Type] : llvm::enumerate(Types)) {
    if (!Type || !Type->Size)
      break;
    const bool Named = Index < FixedCount;
    const auto Parts = floatingPieces(Type, Word);
    if (!Parts && Named)
      break;
    if (!Named && !isIntegerScalar(Type) && !isFloatingScalar(Type) &&
        (!isRecord(Type) || (Type->Passing != NdRecordPassing::ByValue &&
                             Type->Passing != NdRecordPassing::ByReference &&
                             Type->Passing != NdRecordPassing::Complex)))
      break;
    std::vector<SourceParameterPiece> Pieces;
    if (Named && !Parts->empty()) {
      const unsigned Floats =
          llvm::count_if(*Parts, [](auto L) { return L.Floating; });
      if (FP + Floats <= TRI.FPParamRegs.size() &&
          GP + (Parts->size() - Floats) <= TRI.IntParamRegs.size()) {
        for (auto L : *Parts)
          Pieces.push_back(
              L.Floating
                  ? registerPiece(K::FloatingRegister, TRI.FPParamRegs[FP++],
                                  L.Offset, L.Size)
                  : registerPiece(K::IntegerRegister, TRI.IntParamRegs[GP++],
                                  L.Offset, L.Size));
        Result.Parameters.push_back(std::move(Pieces));
        continue;
      }
    }
    const bool Indirect =
        Type->Passing == NdRecordPassing::ByReference || Type->Size > 2 * Word;
    const unsigned Bytes = Indirect ? Word : Type->Size;
    unsigned Align = Indirect ? Word : Type->Alignment;
    if (!Align && !isRecord(Type))
      Align = Bytes;
    if ((!Named && !Align) || (Align && !std::has_single_bit(Align)))
      break;
    // Unnamed arguments aligned to twice XLEN start in an even register pair.
    if (!Named && Bytes > Word && Align >= 2 * Word)
      GP = llvm::alignTo(GP, 2u);
    if (GP >= TRI.IntParamRegs.size()) {
      if (!Align)
        break;
      Stack = llvm::alignTo(Stack, std::clamp(Align, Word, 16u));
    }
    for (unsigned Offset = 0; Offset < Bytes; Offset += Word) {
      const unsigned Part = std::min(Word, Bytes - Offset);
      if (GP < TRI.IntParamRegs.size())
        Pieces.push_back(registerPiece(K::IntegerRegister,
                                       TRI.IntParamRegs[GP++], Offset, Part,
                                       Indirect));
      else {
        Pieces.push_back(stackPiece(Stack, Offset, Part, Indirect));
        Stack += Word;
      }
    }
    Result.Parameters.push_back(std::move(Pieces));
  }
  return Result;
}
