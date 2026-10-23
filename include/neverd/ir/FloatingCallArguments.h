//===- FloatingCallArguments.h - Scalar FP call carriers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The complete transport of scalar and dense HFA floating-point signatures.
/// SourceParameterPlacement owns the target's parameter locations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_FLOATINGCALLARGUMENTS_H
#define NEVERD_IR_FLOATINGCALLARGUMENTS_H

#include "neverd/ir/SourceParameterPlacement.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

struct FloatingCallArguments {
  /// Scalar bytes in consecutive floating parameter registers.
  std::vector<uint16_t> ParameterBytes;
  uint16_t ReturnBytes = 0;
  /// A dense AAPCS64 HFA result has this many equal FP members; zero denotes
  /// a scalar result. ReturnBytes is the complete returned object's size.
  uint16_t ReturnElements = 0;
  /// For an HFA signature, one count per source parameter: zero for a scalar,
  /// otherwise the number of consecutive Scalars carrying that dense record.
  /// A spilled record keeps its dense member offsets within one stack object.
  std::vector<uint16_t> ParameterElements;
  struct ScalarParameter {
    SourceABICarrierKind Kind = SourceABICarrierKind::None;
    uint64_t Register = 0;
    int64_t StackOffset = 0;
    uint16_t Bytes = 0;
    bool Floating = false;
    bool Pointer = false;
    bool operator==(const ScalarParameter &) const = default;
  };
  /// A mixed scalar signature, in source order. Empty retains the consecutive
  /// floating-register representation above. Locations come exclusively from
  /// SourceParameterPlacement, including the entry-SP bias of stack values.
  std::vector<ScalarParameter> Scalars;
  bool operator==(const FloatingCallArguments &) const = default;
};

/// A complete fixed scalar signature. Consecutive floating registers use the
/// compact ParameterBytes form; mixed classes use source-ordered Scalars,
/// including stack locations. Dense AAPCS64 HFAs preserve their complete
/// parameter groups in registers or on the stack. Other records and special
/// conventions need other adapters. The caller establishes the fixed C ABI.
std::optional<FloatingCallArguments>
floatingCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> Parameters);

} // namespace neverd

#endif // NEVERD_IR_FLOATINGCALLARGUMENTS_H
