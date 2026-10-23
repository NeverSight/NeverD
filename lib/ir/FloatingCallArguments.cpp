//===- FloatingCallArguments.cpp - Scalar FP call carriers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate complete scalar floating-point register arguments and results.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/FloatingCallArguments.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

std::optional<FloatingCallArguments>
floatingCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> Parameters) {
  if (A != Arch::X86 && A != Arch::X64 && A != Arch::AArch64)
    return std::nullopt;
  const auto Scalar = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Float &&
           (Type->Size == sizeof(float) || Type->Size == sizeof(double));
  };
  if (Parameters.size() > static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return std::nullopt;
  const auto ScalarParameter = [&](const TypeRef &Type) {
    return Scalar(Type) ||
           (Type &&
            (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
             Type->Size == 8) &&
            (Type->Kind != NdTypeKind::Ptr ||
             Type->Size == getTargetRegInfo(A).PointerSize) &&
            (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr ||
             (Type->Kind == NdTypeKind::Struct && Type->IsEnum)));
  };
  // Keep the original record placement. Flattening the source signature
  // before placement would split an HFA between the last FP register and
  // the stack, or give each stacked float an independent eight-byte slot.
  const auto HFA =
      [&](const TypeRef &Type) -> std::optional<std::vector<NdScalarLeaf>> {
    if (A != Arch::AArch64 || !Type ||
        Type->Passing != NdRecordPassing::ByValue)
      return std::nullopt;
    const auto Leaves = aapcs64FloatingAggregate(Type);
    if (!Leaves || Leaves->empty() ||
        (Leaves->front().Size != 4 && Leaves->front().Size != 8) ||
        Type->Alignment != Leaves->front().Size)
      return std::nullopt;
    unsigned End = 0;
    for (const auto &Leaf : *Leaves) {
      if (Leaf.Offset != End)
        return std::nullopt;
      End += Leaf.Size;
    }
    return End == Type->Size ? Leaves : std::nullopt;
  };
  bool HasRecord = ReturnType && ReturnType->Kind == NdTypeKind::Struct &&
                   !ReturnType->IsEnum;
  for (const auto &Type : Parameters)
    HasRecord |= Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum;
  if (HasRecord) {
    const auto ResultLeaves = HFA(ReturnType);
    if (!Scalar(ReturnType) && !ResultLeaves)
      return std::nullopt;
    const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
    if (!Placement || Placement->ResultPointer ||
        Placement->Parameters.size() != Parameters.size())
      return std::nullopt;
    FloatingCallArguments Result;
    Result.ReturnElements = ResultLeaves ? ResultLeaves->size() : 0;
    Result.ReturnBytes = ReturnType->Size;
    for (size_t I = 0; I < Parameters.size(); ++I) {
      const auto &Type = Parameters[I];
      const auto Leaves = HFA(Type);
      const auto &Pieces = Placement->Parameters[I];
      if (Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum) {
        if (!Leaves)
          return std::nullopt;
        const bool Stacked =
            Pieces.size() == 1 && !Pieces[0].Indirect && !Pieces[0].Offset &&
            Pieces[0].Location.Kind == SourceABICarrierKind::Stack &&
            Pieces[0].Location.ValueBytes == Type->Size;
        if (!Stacked && Pieces.size() != Leaves->size())
          return std::nullopt;
        Result.ParameterElements.push_back(Leaves->size());
        for (size_t J = 0; J < Leaves->size(); ++J) {
          const auto &Leaf = (*Leaves)[J];
          if (Stacked) {
            Result.Scalars.push_back(
                {SourceABICarrierKind::Stack, 0,
                 Pieces[0].Location.EntryStackOffset + Leaf.Offset, Leaf.Size,
                 true, false});
          } else {
            const auto &Piece = Pieces[J];
            if (Piece.Indirect || Piece.Offset != Leaf.Offset ||
                Piece.Location.Kind != SourceABICarrierKind::FloatingRegister ||
                Piece.Location.ValueBytes != Leaf.Size)
              return std::nullopt;
            Result.Scalars.push_back({Piece.Location.Kind,
                                      Piece.Location.RegisterOffset, 0,
                                      Leaf.Size, true, false});
          }
        }
      } else {
        if (!ScalarParameter(Type) || Pieces.size() != 1 ||
            Pieces[0].Indirect || Pieces[0].Offset ||
            Pieces[0].Location.ValueBytes != Type->Size)
          return std::nullopt;
        const auto &Location = Pieces[0].Location;
        if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
            Location.Kind != SourceABICarrierKind::FloatingRegister &&
            Location.Kind != SourceABICarrierKind::Stack)
          return std::nullopt;
        Result.ParameterElements.push_back(0);
        Result.Scalars.push_back(
            {Location.Kind, Location.RegisterOffset, Location.EntryStackOffset,
             Location.ValueBytes, Scalar(Type), Type->Kind == NdTypeKind::Ptr});
      }
      if (Result.Scalars.size() >
          static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
        return std::nullopt;
    }
    return Result;
  }
  if (!Scalar(ReturnType))
    return std::nullopt;
  const auto Registers = getTargetRegInfo(A).floatingParamRegs(F);
  bool AllFloating = true;
  for (const auto &Type : Parameters)
    if (!Scalar(Type)) {
      AllFloating = false;
      if (!ScalarParameter(Type))
        return std::nullopt;
    }
  if (A != Arch::X86 && AllFloating && Parameters.size() > Registers.size())
    return std::nullopt;
  const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
  if (!Placement || Placement->ResultPointer ||
      Placement->Parameters.size() != Parameters.size())
    return std::nullopt;
  FloatingCallArguments Result;
  Result.ReturnBytes = ReturnType->Size;
  if (A == Arch::X86 || !AllFloating) {
    for (size_t I = 0; I < Parameters.size(); ++I) {
      const auto &Pieces = Placement->Parameters[I];
      if (Pieces.size() != 1 || Pieces[0].Indirect || Pieces[0].Offset != 0 ||
          Pieces[0].Location.ValueBytes != Parameters[I]->Size)
        return std::nullopt;
      const auto &Location = Pieces[0].Location;
      if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
          Location.Kind != SourceABICarrierKind::FloatingRegister &&
          Location.Kind != SourceABICarrierKind::Stack)
        return std::nullopt;
      Result.Scalars.push_back({Location.Kind, Location.RegisterOffset,
                                Location.EntryStackOffset, Location.ValueBytes,
                                Scalar(Parameters[I]),
                                Parameters[I]->Kind == NdTypeKind::Ptr});
    }
    return Result;
  }
  for (size_t I = 0; I < Parameters.size(); ++I) {
    const auto &Pieces = Placement->Parameters[I];
    if (Pieces.size() != 1 || Pieces[0].Indirect || Pieces[0].Offset != 0 ||
        Pieces[0].Location.Kind != SourceABICarrierKind::FloatingRegister ||
        Pieces[0].Location.RegisterOffset != Registers[I] ||
        Pieces[0].Location.ValueBytes != Parameters[I]->Size)
      return std::nullopt;
    Result.ParameterBytes.push_back(Parameters[I]->Size);
  }
  return Result;
}

} // namespace neverd
