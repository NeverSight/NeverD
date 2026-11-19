//===- SourceRecordABI.h - MIPS record call transport -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Physical carriers of complete fixed n32/n64 natural C record signatures.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MIPS_SOURCERECORDABI_H
#define NEVERD_IR_MIPS_SOURCERECORDABI_H

#include "neverd/ir/SourceParameterPlacement.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>
#include <vector>

namespace neverd::mips {

struct RecordCallPlacement {
  SourceParameterPlacement Arguments;
  /// Empty for void and for a result written through Arguments.ResultPointer.
  /// A hidden result pointer is an input; it does not define a v0 return.
  std::vector<SourceParameterPiece> Result;
};

/// Classify complete by-value natural structs, including nested structs,
/// arrays and pointers of the target width. Scalar arguments retain the same
/// authority as placeSourceParameters. At least one parameter or the result
/// must be a record. Incomplete/packed/overlapping layouts, complex members,
/// non-C records, and variadic records are not admitted here.
/// This describes transport only; source entry/call publication must also
/// prove its original carriers and complete value reconstruction.
std::optional<RecordCallPlacement>
placeRecordCall(Arch Architecture, BinaryFormat Format,
                const TypeRef &ReturnType, llvm::ArrayRef<TypeRef> Parameters,
                bool Variadic = false);

} // namespace neverd::mips

#endif // NEVERD_IR_MIPS_SOURCERECORDABI_H
