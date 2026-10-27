//===- RegistrationConditionalFrame.h - Bound callee inputs -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check invocation-specific registration frame inputs and bounded effects.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H

#include "RegistrationFrame.h"
#include "RegistrationJumpCapture.h"

#include "neverd/ir/RegistrationCall.h"
#include "neverd/ir/RegistrationState.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;
class RegistrationCallCalleeIndex;
} // namespace neverd

namespace neverd::registration_state {

struct BorrowedJumpResume {
  va_t Address = InvalidVA;
  int Seq = -1;
  va_t CaptureCall = InvalidVA;
  FrameValue Value;
  bool operator==(const BorrowedJumpResume &) const = default;
};

/// Source-only invocation effects. A returned alias is distinct from capture:
/// a consumer must bind its uses before claiming that the caller slot is
/// private. No native entry, call ABI or target-only callee contract is
/// implied.
struct BoundCxxCalleeEffect {
  uint32_t StackPopBytes = 0;
  /// Every supplied borrow, including objects that the callee never accesses.
  std::vector<RegistrationObjectExtent> Objects;
  /// Entry bytes whose initialized values seeded this invocation. A later
  /// read after a checked store is a read effect, not an entry precondition.
  std::vector<RegistrationObjectExtent> RequiredInitializedInputs;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  /// Bytes initialized on every normal return. These carry no value or
  /// must-write claim and grant no initialization on exceptional exits.
  std::vector<RegistrationObjectExtent> InitializedOnReturn;
  std::vector<ExceptionAddressRange> ImageReads;
  struct Return {
    va_t Address = InvalidVA;
    int Seq = -1;
    /// Already projected into the caller domain. An exact frame coordinate
    /// names a checked caller borrow, never the callee's private allocation.
    FrameValue Value = FrameValue::taint(true);
  };
  std::vector<Return> Returns;
};

/// A returning plain helper under the current scalar inputs. Every reached
/// call is excluded, so this result grants no implicit runtime/callback
/// effects. Image reads and the projected EAX value belong to this invocation.
struct BoundPlainCalleeEffect {
  uint32_t StackPopBytes = 0;
  std::vector<ExceptionAddressRange> ImageReads;
  FrameValue ReturnValue = FrameValue::taint(true);
};

/// The current source caller alone owns this storage query. It may extend a
/// scalar word into contiguous live storage after checking its allocation and
/// runtime administration. The query bills that caller's active work counter;
/// the callee must not retain it or infer an extent from neighboring values.
using BoundCxxBorrowStorage =
    std::function<std::optional<RegistrationObjectExtent>(int32_t)>;

using BoundCxxCalleeProver = std::function<std::optional<BoundCxxCalleeEffect>(
    const LowFunc &, const LowOp &, const FrameState &,
    const std::set<int32_t> &, const BoundCxxBorrowStorage &, size_t &)>;

/// Root invocations retain their actual caller domain. All image import slots
/// participate in binding invalidation before this proof can use a callee's
/// personality or nested import, including slots unused by the caller itself.
struct CxxCallerProof {
  BoundCxxCalleeProver ProveCall;
  std::set<va_t> ImportSlots;
};

/// Invocation-specific inputs in the callee's established EBP coordinates.
/// Positive object extents are proved caller storage, disjoint from this
/// invocation's return PC, private frame and incoming argument slots.
struct ConditionalFrame {
  std::function<bool(const LowOp &, const FrameState &,
                     const std::set<int32_t> &, std::optional<FrameValue> &)>
      ProveCall;
  /// Replays a nested C++ invocation after binding its physical CALL and
  /// current inputs. Only the private conditional solver consumes this hook.
  BoundCxxCalleeProver ProveCxxCall;
  std::function<std::optional<BoundPlainCalleeEffect>(
      const LowFunc &, const LowOp &, const FrameState &,
      const std::set<int32_t> &, size_t &)>
      ProvePlainCall;
  /// Optional closed-image memory authority. It checks mapped storage and
  /// records may-effects without supplying the bytes' runtime values.
  std::function<bool(va_t, uint16_t, bool, size_t &)> ProveImageMemory;
  /// Explicit source-only original-layout hypothesis. The ordinary/native
  /// entry points never populate this field; dependent facts require their
  /// separate invocation evidence and final PE validation.
  /// The final flag identifies an existing cookie-protocol value. The hook
  /// observes its address but cannot replace it with original image bits.
  std::function<std::optional<FrameValue>(const LowOp &, const FrameTransfer &,
                                          size_t &, bool)>
      EvaluateOriginalImage;
  /// Full-width volatile GPR facts, indexed like FrameState::Registers. A
  /// partial incoming register never supplies an untainted full-word seed.
  std::map<unsigned, FrameValue> Registers;
  std::map<int32_t, FrameValue> Arguments;
  std::vector<RegistrationObjectExtent> Objects;
  std::map<int32_t, FrameValue> ObjectCells;
  std::map<va_t, SEHJumpCapture> JumpBuffers;
  std::map<std::pair<va_t, int>, BorrowedJumpResume> JumpResumes;
  std::set<std::pair<int32_t, int32_t>> Reads;
  std::set<std::pair<int32_t, int32_t>> Writes;
  std::set<int32_t> UsedArguments;
  /// Physical EAX at each ordinary return. Joins retain possible alias taint;
  /// callback returns belong to another invocation and are excluded.
  std::map<std::pair<va_t, int>, FrameValue> ReturnValues;
  std::map<std::pair<va_t, int>, std::set<int32_t>> ReturnInitializedBytes;
  bool NativeFramePrivate = true;
  bool Complete = true;
  size_t Work = 0;
};

struct BoundCalleeEffect {
  uint32_t StackPopBytes = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  bool HasOrdinaryReturn = true;
  /// Joined physical EAX from complete ordinary returns of this invocation.
  /// This is a source fact, not permission to observe a preserved native call.
  std::optional<FrameValue> ReturnValue;
  std::vector<BorrowedJumpResume> JumpResumes;
  std::optional<RegistrationSEHBoundCall> NativeCall;
};

/// Record native frame observations without weakening the source state proof.
void checkConditionalNativeFrame(const FrameState &State, const LowOp &Op,
                                 const FrameTransfer &Transfer,
                                 ConditionalFrame &Inputs);

/// Prove one exact original helper using its current caller cells. The caller
/// separately binds this target to its authenticated physical CALL occurrence.
/// Initialized, when supplied, owns argument and object byte initialization;
/// a sparse unknown scalar cell is not evidence of missing storage.
std::optional<BoundCalleeEffect>
proveBoundSEHCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, size_t &Work,
                    const std::map<int32_t, SEHJumpCapture> *Captures = nullptr,
                    const std::set<int32_t> *Initialized = nullptr);

/// Inspect a fixed-frame cleanup-only C++ helper for one original direct call.
/// Parent supplies current, initialized caller-frame cells and full-register
/// facts; the caller separately authenticates its CALL instruction boundary
/// and frame ownership, including disjointness from its administration.
/// StackArgumentBytes bounds the supplied incoming area and must match every
/// ordinary RET. Zero requests inference from the complete current near RETs.
/// Borrowed scalar words must be above that area and below the caller's saved
/// EBP, or belong to CallerObjects already borrowed by this caller.
/// CallerObjects grant no ownership to other positive offsets. All code/effects
/// are replayed; invocation-dependent answers must never enter a target-only
/// cache. Initialized, when supplied, proves each input byte separately from
/// its value; an initialized unknown scalar need not have a retained cell.
/// It also permits output-only storage without a value seed: every read must
/// then follow checked initialization within this invocation. Without that
/// explicit byte domain, missing caller cells still supply no borrow.
/// OwnerIndex may reuse only context-free evidence for this same image. It
/// bills its own queries; the caller must also charge Work to that owner once
/// its state analysis finishes. Invocation inputs/results are never cached.
/// BorrowStorage can authorize additional contiguous caller storage. Each
/// word still needs a separate value/initialization check, and a possibly
/// overlapping frame pointer stops the loan before those bytes. Without this
/// current-caller authority, the original one-word storage bound remains.
std::optional<BoundCxxCalleeEffect>
proveBoundCxxCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, uint32_t StackArgumentBytes,
                    size_t &Work,
                    llvm::ArrayRef<RegistrationObjectExtent> CallerObjects = {},
                    const std::set<int32_t> *Initialized = nullptr,
                    RegistrationCallCalleeIndex *OwnerIndex = nullptr,
                    const BoundCxxBorrowStorage *BorrowStorage = nullptr);

using BoundCalleeProver = std::function<std::optional<BoundCalleeEffect>(
    const LowOp &, const FrameState &, size_t &,
    const std::map<int32_t, SEHJumpCapture> &)>;

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H
