//===- SourceParameterPlacementDetail.h - Shared rules ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What every convention's placement rules share: the scalar leaves of a
/// value, the classes of its scalars, and the pieces that record where they
/// arrive.  Each convention's rules are in their own file.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_SOURCEPARAMETERPLACEMENTDETAIL_H
#define NEVERD_LIB_IR_SOURCEPARAMETERPLACEMENTDETAIL_H

#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"

#include <optional>
#include <vector>

namespace neverd::source_placement {

/// An integer, enumeration, Boolean or pointer: what an integer register
/// holds.
bool isIntegerScalar(const TypeRef &Type);
/// A floating-point scalar.
bool isFloatingScalar(const TypeRef &Type);
/// A record (not an enumeration).
bool isRecord(const TypeRef &Type);

/// The scalars of \p Type in offset order, through its records and arrays;
/// none when a part of it has no described layout (a record without a
/// layout or scalar leaves, a vector, a function).
std::optional<std::vector<NdScalarLeaf>> scalarLeaves(const TypeRef &Type);

SourceParameterPiece registerPiece(SourceABICarrierKind Kind, uint64_t Register,
                                   uint16_t Offset, uint16_t Bytes,
                                   bool Indirect = false);
SourceParameterPiece stackPiece(int64_t EntryOffset, uint16_t Offset,
                                uint16_t Bytes, bool Indirect = false);

std::optional<SourceParameterPlacement>
placeSysV(const TargetRegInfo &TRI, BinaryFormat F, const TypeRef &ReturnType,
          llvm::ArrayRef<TypeRef> ParamTypes);
std::optional<SourceParameterPlacement>
placeWin64(const TargetRegInfo &TRI, const TypeRef &ReturnType,
           llvm::ArrayRef<TypeRef> ParamTypes);
std::optional<SourceParameterPlacement>
placeAAPCS64(const TargetRegInfo &TRI, BinaryFormat F,
             const TypeRef &ReturnType, llvm::ArrayRef<TypeRef> ParamTypes,
             bool Variadic = false);
std::optional<SourceParameterPlacement>
placeI386(const TargetRegInfo &TRI, BinaryFormat F, const TypeRef &ReturnType,
          llvm::ArrayRef<TypeRef> ParamTypes);
std::optional<SourceParameterPlacement>
placeI386Entered(const TargetRegInfo &TRI, BinaryFormat F,
                 const TypeRef &ReturnType, llvm::ArrayRef<TypeRef> ParamTypes,
                 llvm::ArrayRef<SourceABIValueLocation> Entries);

} // namespace neverd::source_placement

#endif // NEVERD_LIB_IR_SOURCEPARAMETERPLACEMENTDETAIL_H
