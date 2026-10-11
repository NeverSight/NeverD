//===- SourceParameterPlacementWin64.cpp - Microsoft x64 ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Microsoft x64 convention's parameter passing: each parameter takes the
/// next position whatever its class.  The first four positions are RCX, RDX,
/// R8 and R9, or XMM0-XMM3 for a floating value; later ones are 8-byte stack
/// slots past the home area.  A record of 1, 2, 4 or 8 bytes passes as its
/// bytes, any other by the address of a copy, as do wider scalars and a C++
/// class with a non-trivial copy constructor or destructor; a record result
/// of another size, or such a class, is returned through a hidden pointer
/// in the first position.  A record of the Microsoft C++ ABI, whose class
/// traits PDB does not record, still takes one position, as its bytes or as
/// an address; as a small result it leaves the hidden pointer in doubt.
///
//===----------------------------------------------------------------------===//

#include "SourceParameterPlacementDetail.h"

using namespace neverd;
using namespace neverd::source_placement;

namespace {
constexpr uint16_t kSlotBytes = 8;

/// A record the convention passes or returns in a register, by value.
bool registerSizedRecord(const TypeRef &Type) {
  return isRecord(Type) && (Type->Size == 1 || Type->Size == 2 ||
                            Type->Size == 4 || Type->Size == 8);
}
} // namespace

std::optional<SourceParameterPlacement>
source_placement::placeWin64(const TargetRegInfo &TRI,
                             const TypeRef &ReturnType,
                             llvm::ArrayRef<TypeRef> ParamTypes) {
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  const llvm::ArrayRef<uint64_t> Integer = Layout.Registers;
  const auto Vector = TRI.floatingParamRegs(BinaryFormat::COFF);
  if (Integer.empty() || Vector.size() < Integer.size())
    return std::nullopt;
  SourceParameterPlacement Placement;
  size_t Position = 0;
  auto StackAt = [&](size_t P) {
    return Layout.EntryStackBase +
           static_cast<int64_t>((P - Integer.size()) * kSlotBytes);
  };

  // Compilers return a complex number, a 128-bit integer and a wide long
  // double differently; a result among them leaves every position in doubt.
  if (ReturnType && ReturnType->Size > kSlotBytes &&
      (isIntegerScalar(ReturnType) || isFloatingScalar(ReturnType)))
    return Placement;
  if (isRecord(ReturnType) && ReturnType->Passing == NdRecordPassing::Complex)
    return Placement;
  // A class with a constructor or a non-trivial copy comes back through
  // memory whatever its size; the source must say a small record is not one.
  if (registerSizedRecord(ReturnType) &&
      ReturnType->Passing != NdRecordPassing::ByValue &&
      ReturnType->Passing != NdRecordPassing::ByReference)
    return Placement;
  if (isRecord(ReturnType) &&
      (!registerSizedRecord(ReturnType) ||
       ReturnType->Passing == NdRecordPassing::ByReference)) {
    SourceABIValueLocation Result;
    Result.Kind = SourceABICarrierKind::IntegerRegister;
    Result.RegisterOffset = Integer[Position++];
    Result.ValueBytes = TRI.PointerSize;
    Placement.ResultPointer = Result;
  }

  for (const TypeRef &Type : ParamTypes) {
    if (!Type || !Type->Size ||
        !(isIntegerScalar(Type) || isFloatingScalar(Type) || isRecord(Type)))
      break;
    if (isRecord(Type) && Type->Passing == NdRecordPassing::Complex)
      break;
    const size_t P = Position++;
    const bool InRegister = P < Integer.size();
    const bool Floating = isFloatingScalar(Type) && Type->Size <= kSlotBytes;
    const bool Indirect =
        Type->Size > kSlotBytes ||
        (isRecord(Type) && (!registerSizedRecord(Type) ||
                            Type->Passing == NdRecordPassing::ByReference));
    const uint16_t Bytes = Indirect ? TRI.PointerSize : Type->Size;
    Placement.Parameters.push_back(
        {InRegister ? registerPiece(
                          Floating ? SourceABICarrierKind::FloatingRegister
                                   : SourceABICarrierKind::IntegerRegister,
                          Floating ? Vector[P] : Integer[P], 0, Bytes, Indirect)
                    : stackPiece(StackAt(P), 0, Bytes, Indirect)});
  }
  return Placement;
}
