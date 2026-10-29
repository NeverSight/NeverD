//===- LowUndefinedEffects.h - Architectural arbitrary values ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWUNDEFINEDEFFECTS_H
#define NEVERD_IR_LOW_LOWUNDEFINEDEFFECTS_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
#define NEVERD_LOW_UNDEFINED_VALUE(Name, Value)                                \
  inline constexpr size_t Name = Value;
#define NEVERD_LOW_UNDEFINED_TEXT(Name, Value)                                 \
  inline constexpr char Name[] = Value;
#include "neverd/ir/low/LowUndefinedEffects.def"

/// Whether \p Op fits the six stored input slots of v1 operation identities:
/// at most six operands and default extension slots. Every digest that binds
/// stored slots keeps its v1 bytes only when all of its operations fit.
bool lowOperationFitsV1InputSlots(const LowOp &Op);

/// Bind a sidecar to the exact operation span, including provenance and source
/// coordinates. Six-slot spans retain the v1 identity when extension slots
/// are default; extended operands or stored extension bits select v2. This
/// detects stale reuse; it does not authenticate ISA facts.
std::string lowUndefinedOperationDigest(llvm::ArrayRef<LowOp> Ops);

/// Completeness of the architecture's description of newly arbitrary result
/// bits. An empty Missing record supplies no evidence. Complete covers every
/// architecturally undefined output of this exact instruction, not only flags.
enum class LowUndefinedCoverage : uint8_t { Missing, Complete, Unsupported };

/// An architectural arbitrary value is stable after production and shared by
/// all copies until overwritten. It is neither LLVM undef nor source-language
/// undefined behavior. Every dynamic execution creates a fresh production.
struct LowUndefinedEffect {
  /// Number of completed LowOps, relative to the owning instruction. Apply
  /// after those operations and before the next one; zero means before its
  /// first operation. The value must not exceed the certified OpCount.
  uint64_t AfterOp = 0;
  NdVar Output;
  uint16_t BitOffset = 0;
  uint16_t BitCount = 0;
  /// Absent means unconditional. Otherwise a constant or an instruction-local
  /// temporary already defined at AfterOp must supply an eight-bit container
  /// whose value is proved to be zero or one. Do not reread a mutable physical
  /// count register after the instruction overwrote it.
  std::optional<NdVar> When;
};

/// Owned by the architecture lifter and returned with the exact operation
/// sequence it describes. Ordinary LowIR continues to carry its selected
/// implementation values; this sidecar describes all additional ISA-allowed
/// choices for a separate relational check. Defined and preserved results do
/// not create new arbitrary producers, but retain their input dependencies.
struct LowInstructionUndefinedEffects {
  LowUndefinedCoverage Coverage = LowUndefinedCoverage::Missing;
  uint64_t OpCount = 0;
  /// Required for complete coverage; equal lengths do not imply equal code.
  std::string OperationDigest;
  std::vector<LowUndefinedEffect> Effects;
  std::string Diagnostic;
};

} // namespace neverd

#endif
