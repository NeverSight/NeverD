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
  // One authoritative effect/proof kind; an ordinary returning frame accepts
  // only the exact stack-check failure contract authenticated by its caller.
  enum class TerminationKind { None, RuntimeEntry, StackCheckFailure };
  TerminationKind Termination = TerminationKind::None;
  bool terminates() const { return Termination != TerminationKind::None; }
  // Parameter indexes whose exact private-frame address is borrowed
  // synchronously and read-only by an independently known call contract.
  std::map<size_t, size_t> ReadOnlyFrameParameters;
  // Parameter indexes whose exact private-frame address is borrowed
  // synchronously and overwritten within this bounded extent. The proof
  // invalidates overlapping spill bytes after the call, so writable scratch
  // storage cannot masquerade as restored incoming machine state.
  std::map<size_t, size_t> WritableFrameParameters;
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
/// values and incomplete graphs fail closed. Ordinary ARM64 calls may consume
/// aligned scalar eight-byte stack arguments written completely in the same
/// block. The callee may overwrite the entire incoming argument area, so its
/// slots and padding lose spill and written-byte facts before subsequent
/// restoration or argument checks.
/// An independently inferred ARM64 entry signature may additionally authorize
/// exact eight-byte reads of its scalar incoming stack slots. Such values are
/// unknown input bytes, never saved-register identities or private-frame facts.
/// A separately authenticated ARM64 stack-check failure may end a successorless
/// block after the same transfer checks. At least one reachable normal return
/// is required, and every normal return still restores all incoming state.
/// This does not prove a result type or authorize machine-code rewriting.
bool restoresNativeSourceState(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    std::set<uint64_t> *UsedEntryRegisters = nullptr,
    const SourceFunctionTypeHint *EntrySignature = nullptr);

/// Prove entry-register uses in a single straight-line ARM64 helper ending in
/// an exact declared runtime termination, optionally preceded by one other
/// independently validated runtime call. Reuse the preservation proof's byte
/// identities and private-frame escape checks, including completely written
/// outgoing scalar stack arguments, but do not require a nonexistent return
/// to restore state. Branches, returns and exceptional edges remain rejected.
bool observesTerminalNativeSourceState(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    std::set<uint64_t> &UsedEntryRegisters);

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
