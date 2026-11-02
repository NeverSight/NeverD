//===- IntegerCallArguments.h - Integer source-call carriers -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Map a declared integer/pointer signature, including records, onto the
/// integer-register and stack-slot operands recovered for a machine call.
/// SourceParameterPlacement owns the platform's placement rules; this adapter
/// gives argument recovery and source emission the same operand identities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTEGERCALLARGUMENTS_H
#define NEVERD_IR_INTEGERCALLARGUMENTS_H

#include "neverd/ir/SourceParameterPlacement.h"

namespace neverd {

struct IntegerCallArgumentPiece {
  unsigned Argument = 0;
  uint16_t Offset = 0;
  uint16_t Bytes = 0;
  /// A record adapter may supply a typed FP operand; copy its bits, never
  /// convert its numeric value to the integer storage used for assembly.
  bool Floating = false;
};

struct IntegerCallParameter {
  std::vector<IntegerCallArgumentPiece> Pieces;
  /// The sole operand points to the ABI's by-value copy of this parameter.
  bool Indirect = false;
};

struct IntegerCallArguments {
  std::vector<IntegerCallParameter> Parameters;
  /// Windows x86/x64 returns this caller-owned buffer's address in EAX/RAX.
  /// It is a physical operand, not a source parameter.
  std::optional<unsigned> ResultPointer;
  /// The integer-register prefix actually read by this signature.
  unsigned Registers = 0;
  /// Recovered operand positions, including unused register positions before
  /// a stack argument. They are not a count of source parameters.
  unsigned Positions = 0;
};

/// Complete scalar integer/pointer operands, in registers or on the stack.
/// Locations are in source order; widths exclude ABI slot padding.
/// This argument contract makes no additional claim about result registers.
struct FixedIntegerCallArguments {
  struct Parameter {
    SourceABICarrierKind Kind = SourceABICarrierKind::None;
    uint64_t Register = 0;
    int64_t StackOffset = 0;
    uint16_t Bytes = 0;
    bool operator==(const Parameter &) const = default;
  };
  std::vector<Parameter> Parameters;
  bool operator==(const FixedIntegerCallArguments &) const = default;
};

/// Fixed scalar x64/AArch64 calls whose complete operands bind before SSA.
/// Stack values need explicit loads; register-only calls must not acquire
/// incidental frame stores. Records, floating values, hidden results and other
/// conventions retain separate adapters. The caller authenticates the
/// declaration.
std::optional<FixedIntegerCallArguments>
fixedIntegerCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                          llvm::ArrayRef<TypeRef> Parameters);

/// Describe every parameter or return none. Floating carriers, hidden result
/// pointers outside Windows x86/x64, over-aligned stack values, and incomplete
/// placements are rejected.
/// The result uses recoverCallAbi's integer register/stack operand order.
std::optional<IntegerCallArguments>
integerCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                     llvm::ArrayRef<TypeRef> Parameters);

} // namespace neverd

#endif // NEVERD_IR_INTEGERCALLARGUMENTS_H
