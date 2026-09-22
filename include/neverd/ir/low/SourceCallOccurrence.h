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

/// Direct calls require a full-width constant target. Indirect calls retain a
/// static target only when the actual machine operand is constant.
std::optional<SourceCallOccurrenceKey>
sourceCallOccurrenceKey(const LowOp &Operation);
} // namespace neverd
#endif
