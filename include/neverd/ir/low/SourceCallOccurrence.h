#ifndef NEVERD_IR_LOW_SOURCECALLOCCURRENCE_H
#define NEVERD_IR_LOW_SOURCECALLOCCURRENCE_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

#include <optional>
#include <tuple>

namespace neverd {
struct LowOp;

/// Identity of one physical call occurrence, shared by source analyses. This
/// records no ABI, effect or authority to publish or rewrite a call.
struct SourceCallOccurrenceKey {
  va_t Instruction = 0;
  int Sequence = -1;
  NdOp Opcode = NdOp::CALL;
  std::optional<va_t> StaticTarget;

  bool operator==(const SourceCallOccurrenceKey &) const = default;

  bool operator<(const SourceCallOccurrenceKey &Other) const {
    return std::tie(Instruction, Sequence, Opcode, StaticTarget) <
           std::tie(Other.Instruction, Other.Sequence, Other.Opcode,
                    Other.StaticTarget);
  }
};

/// Identify a physical call with a 32-bit or 64-bit target operand. Indirect
/// calls retain a static target only when the machine operand is constant.
/// Architecture-specific proofs must use the overload below.
std::optional<SourceCallOccurrenceKey>
sourceCallOccurrenceKey(const LowOp &Operation);

/// Additionally require the complete machine call-target width.
std::optional<SourceCallOccurrenceKey>
sourceCallOccurrenceKey(const LowOp &Operation, Arch Architecture);
} // namespace neverd
#endif
