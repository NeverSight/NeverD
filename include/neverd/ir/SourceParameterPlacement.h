//===- SourceParameterPlacement.h - Source parameter placement --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Where a calling convention passes each parameter of a source signature,
/// such as one the debug information describes: the registers and stack
/// slots that hold its pieces.  A recovered function's parameters are those
/// registers and slots in the convention's own order (integer registers,
/// then stack slots, then floating registers), so a source parameter is
/// matched to them by location, never by position: System V passes
/// `f(double x, int n)` with n in RDI and x in XMM0, and a 16-byte record in
/// two registers.  Each convention's rules are one file
/// (SourceParameterPlacementSysV.cpp, ...).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_SOURCEPARAMETERPLACEMENT_H
#define NEVERD_IR_SOURCEPARAMETERPLACEMENT_H

#include "neverd/Common.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// One piece of a source parameter's value and where it arrives.
struct SourceParameterPiece {
  /// An IntegerRegister or FloatingRegister with its RegisterOffset, or the
  /// Stack at EntryStackOffset; ValueBytes is the piece's size.
  SourceABIValueLocation Location;
  /// Where the piece starts in the parameter's value.
  uint16_t Offset = 0;
  /// The location holds the address of a copy of the value (a record the
  /// convention passes by reference), not the value.
  bool Indirect = false;
};

struct SourceParameterPlacement {
  /// The pieces of each parameter, in signature order.  It stops before the
  /// first parameter whose placement these rules do not cover; a parameter
  /// the code never reads still has its pieces.
  std::vector<std::vector<SourceParameterPiece>> Parameters;
  /// The register or slot of the hidden pointer to caller-owned result
  /// storage, when the convention returns the result through one and passes
  /// it among the parameters.
  std::optional<SourceABIValueLocation> ResultPointer;
};

/// AAPCS64's homogeneous floating aggregate classification, shared by
/// parameter placement and complete header-call result transport.
std::optional<std::vector<NdScalarLeaf>>
aapcs64FloatingAggregate(const TypeRef &Type);

/// The placement of the parameters \p ParamTypes, returning \p ReturnType, in
/// the ordinary convention of code for \p A in a \p F image; none when
/// NeverD has no rules for that convention. Variadic placement currently
/// supports Windows ARM64 scalar arguments only. Its complete argument list,
/// including the named prefix, uses the integer bank and then stack slots.
std::optional<SourceParameterPlacement>
placeSourceParameters(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> ParamTypes,
                      bool Variadic = false);

/// The placement of the parameters \p ParamTypes of an i386 function whose
/// debug information says where they enter it, \p Entries
/// (FunctionSym::ParamEntryLocations): one in a register in it, and those in
/// the caller's slots in the slots that follow, in order, while every
/// parameter before has a known place.  A parameter of no certain place has
/// no pieces.  None where the result's hidden pointer takes a place the
/// information does not say.
std::optional<SourceParameterPlacement>
placeEnteredParameters(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                       llvm::ArrayRef<TypeRef> ParamTypes,
                       llvm::ArrayRef<SourceABIValueLocation> Entries);

} // namespace neverd

#endif // NEVERD_IR_SOURCEPARAMETERPLACEMENT_H
