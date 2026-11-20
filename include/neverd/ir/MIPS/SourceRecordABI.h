//===- SourceRecordABI.h - MIPS record call transport -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Physical carriers of complete fixed o32/n32/n64 natural C record signatures.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MIPS_SOURCERECORDABI_H
#define NEVERD_IR_MIPS_SOURCERECORDABI_H

#include "neverd/ir/SourceParameterPlacement.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
class DebugContext;

namespace mips {

struct RecordCallPlacement {
  SourceParameterPlacement Arguments;
  /// Empty for void and for a result written through Arguments.ResultPointer.
  /// The object resides in the storage described by Arguments.ResultPointer.
  std::vector<SourceParameterPiece> Result;
  /// o32 additionally returns the storage address in v0. n32/n64 do not.
  bool ReturnsResultPointer = false;
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

/// A natural record whose complete object representation consists of byte
/// fields.
bool isByteRecord(const TypeRef &Type);

/// A complete record whose independently addressable leaves share a single
/// full integer argument word. The source object uses memory byte order.
bool isPackedRecordWord(const TypeRef &Type,
                        llvm::ArrayRef<SourceABIValueLocation> Locations);

/// An o32 double or long long carried as two words of one source parameter.
bool isSplitRecordScalar(const SourceParameterTypeHint &Parameter);

/// Bind a source declaration whose components can be represented independently
/// in the source IR. Retains the input unchanged when a carrier is unavailable.
bool assignRecordSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                           BinaryFormat Format);

/// Recover a complete by-value declaration at its original ELF entry address.
/// ResultPieces describes the machine body's actual return words even when
/// they combine or split source members. The returned hint independently
/// describes a call whose source leaves can be bound by the source IR.
std::optional<SourceFunctionTypeHint>
recordSourceABI(const BinaryImage &Image, const DebugContext &Debug, va_t Entry,
                std::vector<SourceParameterPiece> *ResultPieces = nullptr);

} // namespace mips
} // namespace neverd

#endif // NEVERD_IR_MIPS_SOURCERECORDABI_H
