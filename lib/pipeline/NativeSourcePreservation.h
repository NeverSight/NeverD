#ifndef NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H
#define NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H

#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/low/LowIR.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace neverd {

struct NativeSourceCallKey {
  va_t Instruction = 0;
  int Sequence = -1;
  NdOp Opcode = NdOp::CALL;
  std::optional<va_t> StaticTarget;

  bool operator<(const NativeSourceCallKey &Other) const {
    return std::tie(Instruction, Sequence, Opcode, StaticTarget) <
           std::tie(Other.Instruction, Other.Sequence, Other.Opcode,
                    Other.StaticTarget);
  }
};

struct NativeSourceCallContract {
  const SourceFunctionTypeHint *Signature = nullptr;
  // Parameter indexes whose exact private-frame address is borrowed
  // synchronously and read-only by an independently known call contract.
  std::map<size_t, size_t> ReadOnlyFrameParameters;
};

using NativeSourceCalls =
    std::map<NativeSourceCallKey, NativeSourceCallContract>;

/// Identify one exact LowIR call occurrence. Direct calls require a static
/// target; indirect calls retain it only when the machine operand is constant.
std::optional<NativeSourceCallKey> nativeSourceCallKey(const LowOp &Operation);

/// Prove that every exit restores the incoming Darwin preserved registers,
/// stack pointer and link register. Calls must already have validated source
/// declarations. Exact private spills may carry these identities across calls;
/// unknown writes invalidate spills; frame-address spills, escaping frame
/// values and incomplete graphs fail closed.
/// This does not prove a result type or authorize machine-code rewriting.
bool restoresNativeSourceState(const LowFunc &Function, Arch Architecture,
                               const NativeSourceCalls &Calls,
                               std::set<uint64_t> *UsedEntryRegisters =
                                   nullptr);

/// Prove the narrower frameless tail-call shape without requiring a synthetic
/// frame reconstruction. The function may not write any preserved, frame,
/// stack, or link register; every native call must be a source-bound tail call.
/// A bounded byte-taint proof additionally rejects passing or storing a value
/// derived from the incoming stack pointer. This preserves the established
/// leaf contract while closing frame-address escape through caller-save
/// registers.
bool preservesNativeSourceLeafState(const LowFunc &Function, Arch Architecture,
                                    const NativeSourceCalls &Calls);

} // namespace neverd
#endif
