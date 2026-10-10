//===- LowPreservedState.h - Native opaque-state evidence -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWPRESERVEDSTATE_H
#define NEVERD_IR_LOW_LOWPRESERVEDSTATE_H

#include "neverd/ir/low/LowUndefinedEffects.h"

namespace neverd {

/// Closed architectural sets, not caller-authored register ranges. V1 covers
/// vector containers 0..31 [0,512), x87 CW [0,16), MXCSR [0,16), and APX
/// R16..R31 [0,64), only where those architectural components exist. It neither
/// enables optional features nor describes modeled GPRs, flags, memory, CET,
/// opmasks, x87 data/status/tag/pointers, or any other unlisted state.
enum class LowPreservedStateSet : uint8_t { None, LegacyIntegerOpaqueV1 };

enum class LowPreservedStateAudit : uint8_t {
  Missing,
  LegacyIntegerV1,
  CetDisabledReadShadowStackV1,
};

/// An instruction-local architecture fact, independent of undefined-output
/// coverage and of a whole-execution undefined-choice quantifier. The trusted
/// decoder/lifter or exact profile owner constructs it from original bytes.
/// Hash validation detects stale evidence; it cannot authenticate a fact
/// supplied by an untrusted caller. Binary APIs reconstruct their own facts.
struct LowInstructionPreservedState {
  LowPreservedStateAudit Audit = LowPreservedStateAudit::Missing;
  LowPreservedStateSet StateSet = LowPreservedStateSet::None;
  uint32_t SemanticsVersion = 0;
  Arch Architecture = Arch::Unknown;
  InstructionMode Mode = InstructionMode::Default;
  va_t Address = InvalidVA;
  uint16_t Size = 0;
  uint64_t OpCount = 0;
  std::string NativeBytesDigest;
  std::string OperationDigest;

  bool operator==(const LowInstructionPreservedState &) const = default;
};

/// Check bounded structural identity, not ISA authority or reachability.
bool matchesLowPreservedState(const LowInstructionPreservedState &Fact,
                              const LowInstructionBoundary &Boundary,
                              llvm::ArrayRef<uint8_t> Bytes,
                              llvm::ArrayRef<LowOp> Ops);
std::string lowPreservedStateDigest(const LowInstructionPreservedState &Fact);

} // namespace neverd

#endif
