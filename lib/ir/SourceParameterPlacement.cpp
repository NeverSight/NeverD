//===- SourceParameterPlacement.cpp - Placement registry ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SourceParameterPlacementDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"

#include <algorithm>
#include <bit>
#include <functional>

using namespace neverd;
using namespace neverd::source_placement;

namespace {
std::optional<SourceParameterPlacement>
placeELFIntegers(const TargetRegInfo &TRI, const TypeRef &ReturnType,
                 llvm::ArrayRef<TypeRef> Types) {
  const unsigned Word = TRI.PointerSize;
  auto Fits = [Word](const TypeRef &T) {
    return isIntegerScalar(T) && T->Size <= Word &&
           std::has_single_bit(unsigned(T->Size));
  };
  if (!ReturnType ||
      (ReturnType->Kind != NdTypeKind::Void && !Fits(ReturnType)))
    return std::nullopt;
  SourceParameterPlacement Result;
  int64_t Stack = TRI.CallStackArgumentBase;
  for (const auto &Type : Types) {
    if (!Fits(Type))
      break;
    const size_t Index = Result.Parameters.size();
    if (Index < TRI.IntParamRegs.size()) {
      Result.Parameters.push_back(
          {registerPiece(SourceABICarrierKind::IntegerRegister,
                         TRI.IntParamRegs[Index], 0, Type->Size)});
    } else {
      const int64_t Offset =
          Stack + (archLittleEndian(TRI.TheArch) ? 0 : Word - Type->Size);
      Result.Parameters.push_back({stackPiece(Offset, 0, Type->Size)});
      Stack += Word;
    }
  }
  return Result;
}
} // namespace

bool source_placement::isIntegerScalar(const TypeRef &Type) {
  if (!Type || !Type->Size)
    return false;
  return (Type->Kind == NdTypeKind::Int && !sourceAArch64VectorBytes(Type)) ||
         Type->Kind == NdTypeKind::Ptr ||
         (Type->Kind == NdTypeKind::Struct && Type->IsEnum);
}

bool source_placement::isFloatingScalar(const TypeRef &Type) {
  return Type && Type->Size && Type->Kind == NdTypeKind::Float &&
         !sourceAArch64VectorBytes(Type);
}

bool source_placement::isRecord(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum;
}

std::optional<std::vector<NdScalarLeaf>>
source_placement::scalarLeaves(const TypeRef &Type, bool *ContainsUnion) {
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
    if (ContainsUnion && Part->IsUnion) {
      *ContainsUnion = true;
      return false;
    }
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
                              llvm::ArrayRef<TypeRef> ParamTypes, bool Variadic,
                              std::optional<unsigned> FixedCount,
                              InstructionMode EntryMode) {
  if (F == BinaryFormat::ELF && hasRISCVDoubleABI(A)) {
    if (FixedCount && (!Variadic || *FixedCount > ParamTypes.size()))
      return std::nullopt;
    return placeRISCVDoubleABI(getTargetRegInfo(A), ReturnType, ParamTypes,
                               FixedCount.value_or(ParamTypes.size()));
  }
  if (FixedCount && (!Variadic || !isMIPS(A) || !*FixedCount ||
                     *FixedCount > ParamTypes.size()))
    return std::nullopt;
  if (Variadic && !isMIPS(A) && (A != Arch::AArch64 || F != BinaryFormat::COFF))
    return std::nullopt;
  const TargetRegInfo &TRI = getTargetRegInfo(A);
  if (F == BinaryFormat::ELF && (isRISCV(A) || isPowerPC(A)))
    return placeELFIntegers(TRI, ReturnType, ParamTypes);
  switch (A) {
  case Arch::X64:
    return F == BinaryFormat::COFF ? placeWin64(TRI, ReturnType, ParamTypes)
                                   : placeSysV(TRI, F, ReturnType, ParamTypes);
  case Arch::AArch64:
    return placeAAPCS64(TRI, F, ReturnType, ParamTypes, Variadic);
  case Arch::ARM:
    return placeAAPCS32(TRI, F, ReturnType, ParamTypes);
  case Arch::MIPS32:
  case Arch::MIPS32EL:
  case Arch::MIPS64O32:
  case Arch::MIPS64O32EL:
  case Arch::MIPS64:
  case Arch::MIPS64N32:
  case Arch::MIPS64N32EL:
  case Arch::MIPS64EL:
    return placeMIPS(TRI, F, ReturnType, ParamTypes,
                     Variadic ? std::optional<unsigned>(
                                    FixedCount.value_or(ParamTypes.size()))
                              : std::nullopt,
                     EntryMode);
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
