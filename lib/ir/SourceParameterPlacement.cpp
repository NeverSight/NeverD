//===- SourceParameterPlacement.cpp - Placement registry ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SourceParameterPlacementDetail.h"

#include "neverd/ir/Limits.h"

#include <algorithm>
#include <functional>

using namespace neverd;
using namespace neverd::source_placement;

namespace {} // namespace

bool source_placement::isIntegerScalar(const TypeRef &Type) {
  if (!Type || !Type->Size)
    return false;
  return Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr ||
         (Type->Kind == NdTypeKind::Struct && Type->IsEnum);
}

bool source_placement::isFloatingScalar(const TypeRef &Type) {
  return Type && Type->Size && Type->Kind == NdTypeKind::Float;
}

bool source_placement::isRecord(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum;
}

std::optional<std::vector<NdScalarLeaf>>
source_placement::scalarLeaves(const TypeRef &Type) {
  std::vector<NdScalarLeaf> Leaves;
  unsigned Budget =
      ::neverd::limits::ir::kSourceParameterPlacementMaxScalarLeaves;
  std::function<bool(const TypeRef &, uint64_t)> Add = [&](const TypeRef &Part,
                                                           uint64_t Base) {
    if (!Part || !Budget || Base + Part->Size > UINT16_MAX)
      return false;
    --Budget;
    if (isIntegerScalar(Part) || isFloatingScalar(Part)) {
      Leaves.push_back(
          {static_cast<uint16_t>(Base), Part->Size, isFloatingScalar(Part)});
      return true;
    }
    if (Part->Kind == NdTypeKind::Array) {
      if (!Part->ElemType || !Part->ElemType->Size ||
          Part->ArrayCount >
              ::neverd::limits::ir::kSourceParameterPlacementMaxScalarLeaves)
        return false;
      for (uint32_t I = 0; I < Part->ArrayCount; ++I)
        if (!Add(Part->ElemType, Base + uint64_t(I) * Part->ElemType->Size))
          return false;
      return true;
    }
    if (!isRecord(Part))
      return false;
    // The record's C layout, or the scalars its debug information lists.
    if (!Part->Fields.empty() &&
        Part->Fields.size() == Part->FieldOffsets.size()) {
      for (size_t I = 0; I < Part->Fields.size(); ++I)
        if (!Add(Part->Fields[I], Base + Part->FieldOffsets[I]))
          return false;
      return true;
    }
    if (Part->ScalarLeaves.empty() ||
        Leaves.size() + Part->ScalarLeaves.size() >
            ::neverd::limits::ir::kSourceParameterPlacementMaxScalarLeaves)
      return false;
    for (const NdScalarLeaf &Leaf : Part->ScalarLeaves) {
      if (Leaf.Offset + Leaf.Size > Part->Size)
        return false;
      Leaves.push_back({static_cast<uint16_t>(Base + Leaf.Offset), Leaf.Size,
                        Leaf.Floating});
    }
    return true;
  };
  if (!Add(Type, 0))
    return std::nullopt;
  std::stable_sort(Leaves.begin(), Leaves.end(),
                   [](const NdScalarLeaf &L, const NdScalarLeaf &R) {
                     return L.Offset < R.Offset;
                   });
  return Leaves;
}

SourceParameterPiece source_placement::registerPiece(SourceABICarrierKind Kind,
                                                     uint64_t Register,
                                                     uint16_t Offset,
                                                     uint16_t Bytes,
                                                     bool Indirect) {
  SourceParameterPiece Piece;
  Piece.Location.Kind = Kind;
  Piece.Location.RegisterOffset = Register;
  Piece.Location.ValueBytes = Bytes;
  Piece.Offset = Offset;
  Piece.Indirect = Indirect;
  return Piece;
}

SourceParameterPiece source_placement::stackPiece(int64_t EntryOffset,
                                                  uint16_t Offset,
                                                  uint16_t Bytes,
                                                  bool Indirect) {
  SourceParameterPiece Piece;
  Piece.Location.Kind = SourceABICarrierKind::Stack;
  Piece.Location.EntryStackOffset = EntryOffset;
  Piece.Location.ValueBytes = Bytes;
  Piece.Offset = Offset;
  Piece.Indirect = Indirect;
  return Piece;
}

std::optional<SourceParameterPlacement>
neverd::placeSourceParameters(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                              llvm::ArrayRef<TypeRef> ParamTypes,
                              bool Variadic) {
  if (Variadic && (A != Arch::AArch64 || F != BinaryFormat::COFF))
    return std::nullopt;
  const TargetRegInfo &TRI = getTargetRegInfo(A);
  switch (A) {
  case Arch::X64:
    return F == BinaryFormat::COFF ? placeWin64(TRI, ReturnType, ParamTypes)
                                   : placeSysV(TRI, F, ReturnType, ParamTypes);
  case Arch::AArch64:
    return placeAAPCS64(TRI, F, ReturnType, ParamTypes, Variadic);
  case Arch::X86:
    return placeI386(TRI, F, ReturnType, ParamTypes);
  default:
    return std::nullopt;
  }
}

std::optional<SourceParameterPlacement>
neverd::placeEnteredParameters(Arch A, BinaryFormat F,
                               const TypeRef &ReturnType,
                               llvm::ArrayRef<TypeRef> ParamTypes,
                               llvm::ArrayRef<SourceABIValueLocation> Entries) {
  if (A != Arch::X86)
    return std::nullopt;
  return placeI386Entered(getTargetRegInfo(A), F, ReturnType, ParamTypes,
                          Entries);
}
