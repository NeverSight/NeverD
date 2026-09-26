//===- X87PartialRemainder.h - Exact concrete FPREM semantics ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X87_PARTIAL_REMAINDER_H
#define NEVERD_IR_LOW_X87_PARTIAL_REMAINDER_H

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

inline constexpr uint16_t X87PartialRemainderConditionMask = 0x4700;
inline constexpr uint16_t X87PartialRemainderC2Mask = 0x0400;

struct X87PartialRemainderResult {
  std::array<uint8_t, 10> Value;
  uint16_t ConditionCodes = 0;
  bool Complete = false;

  uint16_t knownConditionMask() const {
    return Complete ? X87PartialRemainderConditionMask
                    : X87PartialRemainderC2Mask;
  }
};

/// Evaluate one concrete FPREM or FPREM1 step on x87 80-bit encodings. Reject
/// operands requiring exception/tag semantics rather than guessing a result.
/// Partial-result bytes use the deterministic N=63 CPU model used by Unicorn;
/// only C2 is architecture-defined until reduction completes.
std::optional<X87PartialRemainderResult>
evaluateX87PartialRemainder(llvm::ArrayRef<uint8_t> Dividend,
                            llvm::ArrayRef<uint8_t> Divisor,
                            bool RoundNearestEven);

} // namespace neverd

#endif // NEVERD_IR_LOW_X87_PARTIAL_REMAINDER_H
