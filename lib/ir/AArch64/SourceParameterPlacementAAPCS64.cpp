//===- SourceParameterPlacementAAPCS64.cpp - AArch64 ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AArch64 procedure call standard's parameter passing: integer values
/// in X0-X7 (128-bit pairs start at even registers except on Darwin), floating
/// values and homogeneous floating-point aggregate members in V0-V7, records
/// of at most 16 bytes in consecutive X registers, larger ones by the address
/// of a copy.  Values the registers no longer hold go to 8-byte stack slots;
/// Apple's arm64 packs fixed scalar spills by their natural size and
/// alignment, including complete aggregate spills. An indirect
/// result's address travels in X8, apart from the parameters.
///
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "llvm/Support/MathExtras.h"

using namespace neverd;
using namespace neverd::source_placement;

namespace {
#define NEVERD_AAPCS64_PLACEMENT_VALUE(Type, Name, Value)                      \
  constexpr Type Name = Value;
#include "SourceParameterPlacementAAPCS64.def"

} // namespace

std::optional<std::vector<NdScalarLeaf>>
neverd::aapcs64FloatingAggregate(const TypeRef &Record) {
  if (!isRecord(Record))
    return std::nullopt;
  const auto Leaves = scalarLeaves(Record);
  if (!Leaves || Leaves->empty() || Leaves->size() > kMaxAggregateMembers)
    return std::nullopt;
  for (const NdScalarLeaf &L : *Leaves)
    if (!L.Floating || L.Size != Leaves->front().Size)
      return std::nullopt;
  return Leaves;
}

std::optional<SourceParameterPlacement> source_placement::placeAAPCS64(
    const TargetRegInfo &TRI, BinaryFormat F, const TypeRef &ReturnType,
    llvm::ArrayRef<TypeRef> ParamTypes, bool Variadic) {
  if (Variadic && F != BinaryFormat::COFF)
    return std::nullopt;
  // The result's storage address travels in X8, so the result moves no
  // parameter.
  const auto Layout = TRI.integerArgumentLayout(F);
  const llvm::ArrayRef<uint64_t> Integer = Layout.Registers;
  const llvm::ArrayRef<uint64_t> Vector = TRI.FPParamRegs;
  if (Integer.empty() || Vector.empty())
    return std::nullopt;
  SourceParameterPlacement Placement;
  if (isRecord(ReturnType) &&
      (ReturnType->Passing == NdRecordPassing::ByReference ||
       ((ReturnType->Passing == NdRecordPassing::ByValue ||
         ReturnType->Passing == NdRecordPassing::Complex) &&
        ReturnType->Size > kMaxRegisterRecordBytes &&
        (ReturnType->Size > kMaxAggregateBytes || scalarLeaves(ReturnType)) &&
        !aapcs64FloatingAggregate(ReturnType))))
    Placement.ResultPointer =
        registerPiece(SourceABICarrierKind::IntegerRegister,
                      TRI.indirectResultReg(), 0, TRI.PointerSize)
            .Location;
  size_t NextInteger = 0, NextVector = 0;
  int64_t NextStack = Layout.EntryStackBase;
  bool Undescribed = false;
  auto OnStack = [&](uint16_t Bytes, uint16_t Align, bool Indirect = false) {
    std::vector<SourceParameterPiece> Pieces;
    if (!Align) {
      Undescribed = true;
      return Pieces;
    }
    NextStack = static_cast<int64_t>(llvm::alignTo(
        static_cast<uint64_t>(NextStack),
        F == BinaryFormat::MachO ? Align
                                 : std::max<uint16_t>(Align, kRegisterBytes)));
    Pieces.push_back(stackPiece(NextStack, 0, Bytes, Indirect));
    NextStack +=
        F == BinaryFormat::MachO
            ? Bytes
            : static_cast<int64_t>(llvm::alignTo(Bytes, kRegisterBytes));
    return Pieces;
  };
  auto Integers = [&](uint16_t Bytes, uint16_t Align) {
    std::vector<SourceParameterPiece> Pieces;
    const size_t Count = (Bytes + kRegisterBytes - 1) / kRegisterBytes;
    if (Align == kIntegerPairRegisters * kRegisterBytes &&
        F != BinaryFormat::MachO)
      NextInteger = llvm::alignTo(NextInteger, kIntegerPairRegisters);
    if (NextInteger + Count > Integer.size()) {
      NextInteger = Integer.size();
      return OnStack(Bytes, Align);
    }
    for (size_t I = 0; I < Count; ++I) {
      const uint16_t Offset = static_cast<uint16_t>(I * kRegisterBytes);
      Pieces.push_back(registerPiece(
          SourceABICarrierKind::IntegerRegister, Integer[NextInteger++], Offset,
          std::min<uint16_t>(kRegisterBytes, Bytes - Offset)));
    }
    return Pieces;
  };
  auto Address = [&] {
    // The address of a copy, as an integer value.
    return NextInteger < Integer.size()
               ? std::vector{registerPiece(
                     SourceABICarrierKind::IntegerRegister,
                     Integer[NextInteger++], 0, TRI.PointerSize,
                     /*Indirect=*/true)}
               : OnStack(TRI.PointerSize, kRegisterBytes, /*Indirect=*/true);
  };

  for (const TypeRef &Type : ParamTypes) {
    std::vector<SourceParameterPiece> Pieces;
    // Windows variadics use one eight-byte slot per supported scalar, even
    // for the named floating prefix. Preserve the logical source type while
    // describing its raw bits in X0-X7 or the outgoing stack. Aggregates and
    // 128-bit values require their own complete variadic classification.
    if (Variadic) {
      if ((!isIntegerScalar(Type) && !isFloatingScalar(Type)) || !Type->Size ||
          Type->Size > kRegisterBytes)
        break;
      Pieces = Integers(Type->Size, Type->Size);
      Placement.Parameters.push_back(std::move(Pieces));
      continue;
    }
    if (isIntegerScalar(Type) &&
        Type->Size <= kIntegerPairRegisters * kRegisterBytes) {
      Pieces = Integers(Type->Size, Type->Size);
    } else if (isFloatingScalar(Type)) {
      if (NextVector < Vector.size()) {
        Pieces.push_back(registerPiece(SourceABICarrierKind::FloatingRegister,
                                       Vector[NextVector++], 0, Type->Size));
      } else {
        Pieces = OnStack(Type->Size, Type->Size);
      }
    } else if (isRecord(Type) &&
               Type->Passing == NdRecordPassing::ByReference) {
      Pieces = Address();
    } else if (isRecord(Type) && Type->Size &&
               (Type->Passing == NdRecordPassing::ByValue ||
                Type->Passing == NdRecordPassing::Complex)) {
      if (const auto Members = aapcs64FloatingAggregate(Type)) {
        if (NextVector + Members->size() > Vector.size()) {
          NextVector = Vector.size();
          Pieces = OnStack(Type->Size, Type->Alignment);
        } else {
          for (const NdScalarLeaf &Member : *Members)
            Pieces.push_back(registerPiece(
                SourceABICarrierKind::FloatingRegister, Vector[NextVector++],
                Member.Offset, Member.Size));
        }
      } else if (Type->Size > kMaxRegisterRecordBytes) {
        // Without its scalars, a record that could be a homogeneous
        // aggregate has no certain place.
        if (Type->Size <= kMaxAggregateBytes && !scalarLeaves(Type))
          break;
        Pieces = Address();
      } else {
        // A 16-byte-aligned record starts at an even register outside Darwin.
        if (!scalarLeaves(Type) || !Type->Alignment)
          break;
        Pieces = Integers(Type->Size, Type->Alignment);
      }
    } else {
      break;
    }
    if (Undescribed)
      break;
    Placement.Parameters.push_back(std::move(Pieces));
  }
  return Placement;
}
