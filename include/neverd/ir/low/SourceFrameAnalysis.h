#ifndef NEVERD_IR_LOW_SOURCEFRAMEANALYSIS_H
#define NEVERD_IR_LOW_SOURCEFRAMEANALYSIS_H

#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/low/SourceFrameEffects.h"
#include "neverd/ir/low/SourceRegisterCopy.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

using NativeSourceCallKey = SourceCallOccurrenceKey;

struct NativeSourceCallContract : SourceFrameEffects {
  const SourceFunctionTypeHint *Signature = nullptr;
  // Separately authenticated for this operation, never implied by its ABI or
  // the platform's callee-save bank. This preserves only the in/out error
  // value; it grants no memory or frame borrowing permission.
  bool PreservesSwiftErrorResult = false;
  // Mutually exclusive with Signature. The caller must freshly authenticate
  // this exact occurrence against the original image and LowIR.
  const SourceRegisterCopy *RegisterCopy = nullptr;
  // One authoritative effect/proof kind. An ordinary returning frame accepts
  // only exact exceptional exits authenticated by its caller.
  enum class TerminationKind {
    None,
    RuntimeEntry,
    StackCheckFailure,
    SwiftDictionaryViolation
  };
  TerminationKind Termination = TerminationKind::None;
  bool terminates() const { return Termination != TerminationKind::None; }
};

using NativeSourceCalls =
    std::map<NativeSourceCallKey, NativeSourceCallContract>;

/// Identify one exact LowIR call occurrence. Direct calls require a static
/// target; indirect calls retain it only when the machine operand is constant.
std::optional<NativeSourceCallKey> nativeSourceCallKey(const LowOp &Operation);

/// True only for a pure intrinsic whose complete scalar input/output shape is
/// sufficient for native source ABI inference and preservation transfer.
bool hasNativeScalarIntrinsicEvidence(const LowOp &Operation,
                                      Arch Architecture);

/// A well-formed alignment guard changes no bytes on its normal continuation.
/// Display observations may cross it while retaining the original guard. This
/// grants no alignment, no-fault, restoration or source-rewrite certificate.
bool isSourceFrameObservationGuard(const LowOp &Operation);

/// Total scalar bit transfers with no observable effect beyond their outputs.
/// This opcode query neither validates operands nor proves address roles or
/// restored state. Division, floating operations and intrinsics remain outside
/// it because an unused result alone cannot discard faults or status effects.
bool isSourceFrameScalarTransfer(NdOp Opcode);

/// Prove that every exit restores the format-specific preserved registers,
/// stack pointer and link register. Calls must already have validated source
/// declarations. Exact private spills may carry these identities across calls;
/// unknown writes invalidate spills; frame-address spills, escaping frame
/// values and incomplete graphs fail closed. Ordinary ARM64 calls may consume
/// aligned scalar eight-byte stack arguments written completely in the same
/// block. The callee may overwrite the entire incoming argument area, so its
/// slots and padding lose spill and written-byte facts before subsequent
/// restoration or argument checks.
/// An independently inferred entry signature may additionally authorize exact
/// eight-byte reads of scalar incoming stack slots: ARM64 at nonnegative
/// offsets, or PE/ELF x64 above the pushed return address. Such values are
/// unknown input bytes, never saved-register identities or private-frame facts.
/// Separately authenticated ARM64 exceptional calls may end a successorless
/// block after the same transfer checks. At least one reachable normal return
/// is required, and every normal return still restores all incoming state.
/// UsedEntryRegisters reports complete observed general-register entry words,
/// including volatile inputs forwarded through a call's physical ABI. Calls
/// invalidate volatile identities; exact private spills can preserve them.
/// These use facts neither add restoration obligations nor declare parameters.
/// A complete explicit entry scalar in ARM64 x8 can be forwarded unchanged to
/// an independently authenticated indirect-result producer. Every entry byte
/// must retain its identity on that path. This checks external result writes
/// only; it grants neither private-frame initialization nor a logical entry
/// record return. Ordinary call lowering retains those writes.
/// This does not prove a result type or authorize machine-code rewriting.
bool restoresNativeSourceState(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    std::set<uint64_t> *UsedEntryRegisters = nullptr,
    const SourceFunctionTypeHint *EntrySignature = nullptr,
    BinaryFormat Format = BinaryFormat::MachO);

/// Prove entry-register uses in a single straight-line ARM64 helper ending in
/// an exact declared runtime termination, optionally preceded by one other
/// independently validated runtime call. Reuse the preservation proof's byte
/// identities and private-frame escape checks, including completely written
/// outgoing scalar stack arguments, but do not require a nonexistent return
/// to restore state. Branches, returns and exceptional edges remain rejected.
bool observesTerminalNativeSourceState(const LowFunc &Function,
                                       Arch Architecture,
                                       const NativeSourceCalls &Calls,
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

/// Private-frame proof used by the source publisher, without pipeline state.
bool certifiesPrivateSourceFrame(const LowFunc &Function, Arch Architecture,
                                 bool ExternalMemoryDisjoint,
                                 int64_t *RequiredFrameSize);

/// An opaque, complete LowIR definition, not an address or a call
/// declaration. Consumers must authenticate its original instruction before
/// interpreting these bytes as image data or code pointers.
struct SourceFrameDefinition {
  int BlockId;
  size_t OperationIndex;
  va_t Instruction;
  int Sequence;
  NdVar Output;
  bool operator==(const SourceFrameDefinition &) const = default;
};

struct SourceFrameLoadDefinition {
  SourceFrameDefinition Definition;
  int64_t FrameOffset;
  bool operator==(const SourceFrameLoadDefinition &) const = default;
};

/// Query one complete eight-byte private-frame LOAD. All reaching CFG paths
/// must retain the same ordered bytes and must not have exposed a frame
/// address. Reuses the preservation proof's call ABI, conditional scratch,
/// bounded borrow and frame-or-external rules. Cycles require convergence of
/// every reaching state and a producer in the acyclic entry prefix; effects
/// after a cyclic query still affect its next iteration. Partial or expired
/// definitions, repeated producers, retained scratch and uncertain aliases
/// fail closed. A later end cannot discharge a prefix's lifetime obligation.
/// Calls unable to reach the query need no contract; this grants no source
/// gate. Calls must be freshly authenticated against the current image and
/// LowIR.
/// This query additionally accepts complete scalar/HFA stack members, using
/// their actual widths and invalidating the entire outgoing argument area.
/// Format selects preserved bytes; WorkRemaining optionally shares a budget
/// across queries in one analysis rather than resetting it for every load.
std::optional<SourceFrameLoadDefinition> sourceFrameLoadedDefinition(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    int BlockId, size_t OperationIndex,
    BinaryFormat Format = BinaryFormat::MachO, size_t *WorkRemaining = nullptr);

struct SourceFrameStoredByte {
  bool Initialized = false;
  std::optional<uint8_t> Constant;
  std::optional<SourceFrameDefinition> Definition;
  unsigned DefinitionByte = 0;
  bool operator==(const SourceFrameStoredByte &) const = default;
};

struct SourceFrameArgumentStorage {
  int64_t FrameOffset;
  std::vector<SourceFrameStoredByte> Bytes;
  bool operator==(const SourceFrameArgumentStorage &) const = default;
};

/// Observe bounded private storage immediately before one exact pointer
/// argument is passed. Reuses the reaching-byte proof; every path must agree
/// on the frame address and initialized bytes. Definitions remain opaque and
/// require original-image authentication by their consumer. Uninitialized
/// bytes have no value evidence. Frame pointers, live opaque values, retained
/// scratch and cyclic observations are rejected. This grants no call memory
/// effect, lifetime contract, result layout or source gate.
std::optional<SourceFrameArgumentStorage> sourceFrameCallArgumentStorage(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    const NativeSourceCallKey &Site, size_t Parameter, size_t Bytes);

/// Observe initialized private byte prefixes reached by physical pointers
/// immediately before one original operation. This is a display observation,
/// not an argument ABI, a memory-effect certificate or permission to rewrite
/// storage. X64 and ARM64 use the same reaching-byte and escape proof. No
/// preceding call is admitted; unknown external stores discard old frame
/// bytes. Cyclic observations and expired storage fail closed. Every opaque
/// definition still requires authentication by the consumer. Each prefix ends
/// at uninitialized storage, a frame pointer, the entry-SP boundary or
/// MaxBytes. WorkRemaining shares a cumulative budget across observations.
std::vector<SourceFrameArgumentStorage> sourceFrameStorageBefore(
    const LowFunc &Function, Arch Architecture, BinaryFormat Format,
    int BlockId, size_t OperationIndex, const std::vector<NdVar> &Pointers,
    size_t MaxBytes, size_t *WorkRemaining = nullptr);

struct SourceFrameByValueCopy {
  size_t Parameter;
  int64_t FrameOffset;
  size_t Bytes;
  bool operator==(const SourceFrameByValueCopy &) const = default;
};

/// Prove complete initialized private copies at one original call occurrence.
/// All reaching paths, later uses, frame restoration and retained scratch
/// obligations must pass the shared byte analysis. A consumed copy loses
/// initialization and saved-byte identities; it cannot be read again until
/// overwritten. Copy ranges may not alias another borrowed argument or a
/// hidden result. Effects and the complete entry/call ABIs must be
/// independently authenticated by the caller; this proves no machine identity
/// or source gate.
std::optional<std::vector<SourceFrameByValueCopy>>
sourceFrameByValueCopies(const LowFunc &Function, Arch Architecture,
                         const NativeSourceCalls &Calls,
                         const NativeSourceCallKey &Site,
                         const SourceFunctionTypeHint &EntrySignature);

/// Prove a synchronous, nonescaping complete output prefix through one exact
/// scalar pointer in an ARM64 C void entry. The acyclic body may have at most
/// 128 blocks and write at most 256 bytes; other entry parameters must be
/// eight-byte floating scalars. Every normal return must write every byte of
/// the same prefix and restore state. Output reads, unknown output addresses,
/// pointer spills/escapes, other external stores and conflicting paths refuse.
/// The caller must independently authenticate the current machine, complete
/// entry/call ABIs and effects, and lend an aligned private pointer. External
/// memory origins are assumed disjoint from that caller's private frame.
/// This proves written bytes only, never a result type, byte values, complete
/// source body or publication/dependency closure.
std::optional<size_t> sourceFrameCompleteOutputBytes(
    const LowFunc &Function, Arch Architecture, const NativeSourceCalls &Calls,
    const SourceFunctionTypeHint &EntrySignature, size_t Parameter);

} // namespace neverd
#endif
