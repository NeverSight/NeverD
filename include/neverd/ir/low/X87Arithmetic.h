//===- X87Arithmetic.h - Portable finite binary80 arithmetic ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A numerical building block, not an instruction executor. Raw canonical
/// finite operands are rounded once to x87 precision with the extended exponent
/// range. Stack, pending exceptions, fault delivery and memory remain with the
/// context owner; this function does not admit a portable execution profile.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X87ARITHMETIC_H
#define NEVERD_IR_LOW_X87ARITHMETIC_H

#include "neverd/ir/X87Binary80.h"

#include <cstdint>
#include <optional>

namespace neverd {

enum class X87ArithmeticOperation { Add, Subtract, Multiply, Divide };
enum class X87Comparison { Less, Equal, Greater };

struct X87ComparisonResult {
  X87Comparison Relation;
  uint16_t Exceptions = 0;
};

/// Ordered comparison of canonical finite operands. Signed zeros compare
/// equal; subnormal inputs raise DE. NaNs, infinities and noncanonical inputs
/// remain outside this slice. Stack/flags/condition codes belong to the owner.
std::optional<X87ComparisonResult>
evaluateFiniteX87Compare(const X87Binary80Bytes &Left,
                         const X87Binary80Bytes &Right);

struct X87ArithmeticResult {
  X87Binary80Bytes Value{};
  /// New IE/DE/ZE/OE/UE/PE bits; the context owns sticky status and masking.
  uint16_t Exceptions = 0;
  /// Whether rounding increased the absolute significand (the numerical C1).
  bool RoundedUp = false;
};

/// Reject noncanonical/nonfinite inputs, reserved control bits and PC=01.
/// Finite arithmetic supports PC=24/53/64 and all four RC values. It returns
/// masked numerical results, including indefinite for 0/0 and overflow
/// infinity; callers must not commit these results for unmasked exceptions
/// without a separately qualified state/exception contract. No host FP
/// instructions or host floating types participate in the calculation.
std::optional<X87ArithmeticResult>
evaluateFiniteX87Arithmetic(X87ArithmeticOperation Operation,
                            const X87Binary80Bytes &Left,
                            const X87Binary80Bytes &Right, uint16_t Control);

/// Exact pure integer-to-binary80 conversion shared by typed LowIR and FILD.
/// Accept 1/2/4/8-byte integers; no PC/RC or status effects belong to this
/// conversion. The instruction owner supplies waiting, stack and C1 effects.
std::optional<X87Binary80Bytes>
convertIntegerToX87(uint64_t Bits, unsigned Bytes, bool Signed);

} // namespace neverd

#endif // NEVERD_IR_LOW_X87ARITHMETIC_H
