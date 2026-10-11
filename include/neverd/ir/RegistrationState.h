//===- RegistrationState.h - Checked x86 EH state flow -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONSTATE_H
#define NEVERD_IR_REGISTRATIONSTATE_H

#include "neverd/ir/RegistrationCall.h"
#include "neverd/loader/ExceptionCommon.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

struct LowFunc;

/// A possible language search at a reaching state. ExitedCatches counts CRT
/// catch guards unwound before the target catch is entered. Zero denotes a
/// parent search or a try inside the current catch invocation.
struct RegistrationCxxSearch {
  int32_t Level = -1;
  uint32_t TryIndex = 0;
  uint32_t ExitedCatches = 0;
  auto operator<=>(const RegistrationCxxSearch &) const = default;
};

/// Reaching registration states at an exact, decoded LowIR block. These are
/// CFG facts, not an IP-to-state table in the input image. Keep them separate
/// from the loader descriptor authenticated by the rewrite transaction.
struct RegistrationBlockState {
  int BlockId = -1;
  ExceptionAddressRange Range;
  std::vector<int32_t> Levels;
  bool Unknown = false;
  /// Runtime-invoked filter/cleanup code has its own dispatch context. Its
  /// instructions must not be included in the parent's lexical try interval.
  bool CallbackOnly = false;
  /// Finally callbacks can dispatch to outer scopes even though they are not
  /// lexical parent blocks. Searching filters have a separate runtime context.
  bool CanDispatch = false;
  /// Minimum for the innermost catch guard's search. A failed search proceeds
  /// through enclosing guards and the parent registration. CxxSearches, not
  /// this minimum alone, owns all possible dispatch targets.
  int32_t CxxMinimumTryLevel = 0;
  /// Reached by ordinary flow, runtime dispatch or a checked catch resume.
  /// Empty levels can also describe a reached pre-install/post-remove block.
  /// Consumers require the complete state/lifetime proof before pruning.
  bool Reached = false;
  std::vector<RegistrationCxxSearch> CxxSearches;
};

/// A PE32 C++ catch returns a continuation code pointer to the runtime. This
/// is distinct from a scalar return from the parent function and from an IP
/// map in the input image. CFG construction must decode and replay TargetVA
/// before the continuation closure can authorize native lowering.
struct RegistrationCxxContinuation {
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  va_t TargetVA = InvalidVA;
  /// The runtime snapshot captured before dispatch, relative to established
  /// EBP. Returning from this catch writes that pointer back to SavedESP
  /// (RegistrationOffset - 4) before restoring ESP and jumping to TargetVA.
  /// LowIR and MedIR retain this implicit memory effect on the exact RETURN;
  /// HighIR and native LLVM lowering must materialize it in the source frame.
  int32_t SavedStackOffset = 0;
  bool operator==(const RegistrationCxxContinuation &) const = default;
};

/// An exact LowIR operation that accesses the runtime chain head. Native
/// lowering may replace chain administration only by matching these source
/// occurrence identities; a function-wide completeness bit is insufficient.
struct RegistrationChainAccess {
  enum class Kind : uint8_t {
    ReadPreviousHead,
    ReadInstalledHead,
    Install,
    Remove,
  };
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  Kind AccessKind = Kind::ReadPreviousHead;
};

/// Frame-derived values at exact source occurrences. The shared transfer
/// follows register aliases and frame spills; a missing offset retains the
/// possible frame identity rather than turning it into an unrelated scalar.
struct RegistrationFrameValue {
  va_t Address = InvalidVA;
  int OpSeq = -1;
  std::optional<int32_t> EstablishedFrameOffset;
};

/// An incoming caller-stack memory access, relative to established source EBP.
/// Keep its exact occurrence so native callbacks can access the real caller's
/// stack at the original execution point instead of reading an unseeded copy.
struct RegistrationIncomingFrameAccess {
  va_t Address = InvalidVA;
  int OpSeq = -1;
  int32_t Offset = 0;
  uint16_t Width = 0;
  bool Write = false;
  bool operator==(const RegistrationIncomingFrameAccess &) const = default;
};

/// A source call whose authenticated leaf checker receives the exact decoded
/// image cookie in ECX on every reaching path. Only this occurrence may be
/// replaced by a compiler-owned physical cookie check.
struct RegistrationCookieCheck {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
};

/// A typed scalar catch whose runtime object width follows the checked
/// ThrowInfo contract. A reference home stores a CRT object pointer; the
/// exception allocation is distinct from the private establisher frame.
struct RegistrationCxxCatchObject {
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  va_t TypeDescriptorVA = 0;
  uint32_t ObjectSize = 0;
  int32_t FrameOffset = 0;
  bool Reference = false;
  bool operator==(const RegistrationCxxCatchObject &) const = default;
};

struct RegistrationRuntimeObjectAccess {
  va_t Address = InvalidVA;
  int OpSeq = -1;
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  int32_t Offset = 0;
  uint16_t Width = 0;
  bool Write = false;
  bool operator==(const RegistrationRuntimeObjectAccess &) const = default;
};

struct RegistrationStateAnalysis {
  std::vector<RegistrationBlockState> Blocks;
  bool Complete = false;
  bool CallbackStatesComplete = true;
  bool RegistrationLifetimeComplete = false;
  bool CxxContinuationsComplete = true;
  std::vector<RegistrationCxxContinuation> CxxContinuations;
  /// Complete chain access ownership, including decoded boundaries and
  /// operation identities, available only with a complete registration
  /// lifetime and callback state proof. This is not a native output receipt.
  bool ChainOperationsComplete = false;
  std::vector<RegistrationChainAccess> ChainAccesses;
  std::vector<RegistrationFrameValue> FrameValues;
  bool IncomingFrameAccessesComplete = true;
  std::vector<RegistrationIncomingFrameAccess> IncomingFrameAccesses;
  /// Per-occurrence stack and initialized-object projections, rechecked from
  /// callee evidence across every reaching ordinary and runtime path.
  bool CallFrameEffectsComplete = false;
  std::vector<RegistrationCalleeFrameContract> CalleeContracts;
  std::vector<RegistrationCallFrameEffect> CallFrameEffects;
  bool CleanupFrameEffectsComplete = false;
  std::vector<RegistrationCleanupFrameContract> CleanupContracts;
  std::vector<RegistrationCleanupFrameEffect> CleanupFrameEffects;
  bool CxxCatchObjectsComplete = false;
  std::vector<RegistrationCxxCatchObject> CxxCatchObjects;
  bool RuntimeObjectAccessesComplete = false;
  std::vector<RegistrationRuntimeObjectAccess> RuntimeObjectAccesses;
  /// EH4 source initialization, encoding and immutable cookie lifetime have
  /// been checked across every ordinary and runtime path. This does not prove
  /// the compiler's physical cookie; native codegen validates that separately.
  bool SecurityCookiesComplete = false;
  va_t SecurityCookieVA = 0;
  va_t CookieCheckVA = 0;
  std::vector<RegistrationCookieCheck> CookieChecks;
  /// May-read image extents across all reachable ordinary and runtime roots.
  /// Exact frame-relative reads cannot alias these extents. Unknown addresses
  /// make the set incomplete rather than silently omitting a possible alias.
  bool ImageReadsComplete = false;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<std::string> Diagnostics;

  const RegistrationCallFrameEffect *callFrameEffect(va_t Address,
                                                     int Seq) const {
    const auto Key = std::make_pair(Address, Seq);
    auto It = std::lower_bound(CallFrameEffects.begin(), CallFrameEffects.end(),
                               Key, [](const auto &Call, auto Identity) {
                                 return std::make_pair(Call.Address,
                                                       Call.OpSeq) < Identity;
                               });
    return It != CallFrameEffects.end() &&
                   std::make_pair(It->Address, It->OpSeq) == Key
               ? &*It
               : nullptr;
  }

  const RegistrationCxxContinuation *cxxContinuation(va_t Address,
                                                     int Seq) const {
    const auto Key = std::make_pair(Address, Seq);
    auto It = std::lower_bound(CxxContinuations.begin(), CxxContinuations.end(),
                               Key, [](const auto &Resume, auto Identity) {
                                 return std::make_pair(Resume.Address,
                                                       Resume.OpSeq) < Identity;
                               });
    return It != CxxContinuations.end() &&
                   std::make_pair(It->Address, It->OpSeq) == Key
               ? &*It
               : nullptr;
  }

  const RegistrationCookieCheck *cookieCheck(va_t Address, int Seq) const {
    const auto Key = std::make_pair(Address, Seq);
    auto It = std::lower_bound(CookieChecks.begin(), CookieChecks.end(), Key,
                               [](const auto &Check, auto Identity) {
                                 return std::make_pair(Check.Address,
                                                       Check.OpSeq) < Identity;
                               });
    return It != CookieChecks.end() &&
                   std::make_pair(It->Address, It->OpSeq) == Key
               ? &*It
               : nullptr;
  }

  const RegistrationIncomingFrameAccess *incomingFrameAccess(va_t Address,
                                                             int Seq) const {
    const auto Key = std::make_pair(Address, Seq);
    auto It = std::lower_bound(
        IncomingFrameAccesses.begin(), IncomingFrameAccesses.end(), Key,
        [](const auto &Access, auto Identity) {
          return std::make_pair(Access.Address, Access.OpSeq) < Identity;
        });
    return It != IncomingFrameAccesses.end() &&
                   std::make_pair(It->Address, It->OpSeq) == Key
               ? &*It
               : nullptr;
  }
};

/// Solve ordinary and runtime-dispatch state transfers together. Stores take
/// effect only after their exact decoded instruction retires. A union at a
/// join is retained; address order never selects a predecessor's state.
RegistrationStateAnalysis analyzeRegistrationStates(
    const LowFunc &Function, va_t SecurityCookieVA = 0, va_t CookieCheckVA = 0,
    const std::vector<RegistrationCalleeFrameContract> *Callees = nullptr,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups = nullptr,
    const std::vector<RegistrationCalleeStackContract> *Stacks = nullptr,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds = nullptr);

/// Return exact address intervals only if every reaching state agrees about
/// membership. An ambiguous join cannot be flattened into a lexical try range.
template <typename Predicate>
std::optional<std::vector<ExceptionAddressRange>>
registrationRangesWhere(const RegistrationStateAnalysis &Analysis,
                        Predicate Contains) {
  if (!Analysis.Complete)
    return std::nullopt;
  std::vector<ExceptionAddressRange> Ranges;
  for (const RegistrationBlockState &Block : Analysis.Blocks) {
    if (Block.CallbackOnly)
      continue;
    if (Block.Unknown)
      return std::nullopt;
    if (Block.Levels.empty())
      continue;
    const bool Included = Contains(Block.Levels.front());
    for (int32_t Level : Block.Levels)
      if (Contains(Level) != Included)
        return std::nullopt;
    if (Included)
      Ranges.push_back(Block.Range);
  }
  std::sort(Ranges.begin(), Ranges.end(),
            [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
              return A.Begin < B.Begin;
            });
  std::vector<ExceptionAddressRange> Merged;
  for (const ExceptionAddressRange &Range : Ranges) {
    if (!Range.isValid() ||
        (!Merged.empty() && Range.Begin < Merged.back().End))
      return std::nullopt;
    if (!Merged.empty() && Merged.back().End == Range.Begin)
      Merged.back().End = Range.End;
    else
      Merged.push_back(Range);
  }
  return Merged;
}

} // namespace neverd

#endif // NEVERD_IR_REGISTRATIONSTATE_H
