//===- RegistrationConditionalFrame.h - Bound callee inputs -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCONDITIONALFRAME_H
/// \file
/// Check invocation-specific registration frame inputs and bounded effects.

#include "RegistrationFrame.h"
#include "RegistrationJumpCapture.h"

#include "neverd/ir/RegistrationCall.h"
#include "neverd/ir/RegistrationState.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::registration_state {

struct BorrowedJumpResume {
  va_t Address = InvalidVA;
  int Seq = -1;
  va_t CaptureCall = InvalidVA;
  FrameValue Value;
  bool operator==(const BorrowedJumpResume &) const = default;
};

/// Invocation-specific inputs in the callee's established EBP coordinates.
/// Positive object extents are proved caller storage, disjoint from this
/// invocation's return PC, private frame and incoming argument slots.
struct ConditionalFrame {
  std::function<bool(const LowOp &, const FrameState &)> ProveCall;
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
  bool NativeFramePrivate = true;
  bool Complete = true;
  size_t Work = 0;
};

/// Source-only invocation effects. A returned alias is distinct from capture:
/// a consumer must bind its uses before claiming that the caller slot is
/// private. No native entry, call ABI or target-only callee contract is
/// implied.
struct BoundCxxCalleeEffect {
  uint32_t StackPopBytes = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  struct Return {
    va_t Address = InvalidVA;
    int Seq = -1;
    bool MayAliasFrame = true;
    std::optional<int32_t> BorrowedOffset;
  };
  std::vector<Return> Returns;
};

struct BoundCalleeEffect {
  uint32_t StackPopBytes = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  bool HasOrdinaryReturn = true;
  std::vector<BorrowedJumpResume> JumpResumes;
  std::optional<RegistrationSEHBoundCall> NativeCall;
};

/// Record native frame observations without weakening the source state proof.
void checkConditionalNativeFrame(const FrameState &State, const LowOp &Op,
                                 const FrameTransfer &Transfer,
                                 ConditionalFrame &Inputs);

/// Prove one exact original helper using its current caller cells. The caller
/// separately binds this target to its authenticated physical CALL occurrence.
std::optional<BoundCalleeEffect> proveBoundSEHCallee(
    const BinaryImage &Image, const LowOp &Op, const FrameState &Parent,
    size_t &Work, const std::map<int32_t, SEHJumpCapture> *Captures = nullptr);

/// Inspect a fixed-frame cleanup-only C++ helper for one original direct call.
/// Parent supplies current, initialized caller-frame cells and full-register
/// facts; the caller separately authenticates its CALL instruction boundary
/// and frame ownership, including disjointness from its administration.
/// StackArgumentBytes bounds the supplied incoming area and must match every
/// ordinary RET. Borrowed scalar words must be above that area and below the
/// caller's saved EBP. All code/effects
/// are replayed; invocation-dependent answers must never enter a target-only
/// cache.
std::optional<BoundCxxCalleeEffect>
proveBoundCxxCallee(const BinaryImage &Image, const LowOp &Op,
                    const FrameState &Parent, uint32_t StackArgumentBytes,
                    size_t &Work);

using BoundCalleeProver = std::function<std::optional<BoundCalleeEffect>(
    const LowOp &, const FrameState &, size_t &,
    const std::map<int32_t, SEHJumpCapture> &)>;

} // namespace neverd::registration_state

#endif
