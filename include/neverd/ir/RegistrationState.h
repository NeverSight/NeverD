//===- RegistrationState.h - Checked x86 EH state flow ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe checked source registration states and separate native receipts.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONSTATE_H
#define NEVERD_IR_REGISTRATIONSTATE_H

#include "neverd/ir/RegistrationCall.h"
#include "neverd/loader/ExceptionCommon.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
namespace coff_loader {
struct X86CxxHandlerABI;
struct X86CxxPrologueCall;
} // namespace coff_loader

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
  /// Individually owned levels when SharedLevels is absent. Consumers use
  /// levels() so an unknown domain need not be copied into every block.
  std::vector<int32_t> ExplicitLevels;
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
  /// Possible live catch invocations, outermost first. Snapshot differences
  /// with the same invocation identities collapse here; the LowIR solver
  /// still keeps their frame facts separate. Native projection requires a
  /// unique stack and checks every ordinary and runtime continuation edge.
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> CxxCatchStacks;
  /// Immutable storage owned by the published result, never borrowed from a
  /// solver or loader. Sharing levels does not share frame, search or lifetime
  /// facts and does not change Unknown or any native-admission requirement.
  std::shared_ptr<const std::vector<int32_t>> SharedLevels;

  const std::vector<int32_t> &levels() const {
    return SharedLevels ? *SharedLevels : ExplicitLevels;
  }
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
  /// A nonzero callback entry makes SavedStackOffset relative to that
  /// suspended invocation's stack, rather than the parent establisher.
  va_t SavedCallbackVA = 0;
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
/// image cookie in ECX on every reaching path. Replacing this occurrence still
/// requires the native frame protocol's independent output proof.
struct RegistrationCookieCheck {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
};

/// A C++ handler's decoded cookie condition projected into the parent's
/// established frame coordinate. This is not a physical output-frame offset.
struct RegistrationCxxCookieCondition {
  int32_t FrameOffset = 0;
  int32_t XORFrameOffset = 0;
  bool operator==(const RegistrationCxxCookieCondition &) const = default;
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

/// One source LOAD whose runtime storage belongs to an EH3/EH4 search filter.
/// The slot names its current EXCEPTION_POINTERS; the remaining fields are
/// the record link and read-only scalar exception code/flags. This evidence
/// does not authorize a replacement runtime object in native code.
struct RegistrationSEHExceptionAccess {
  enum class Kind : uint8_t {
    PointersSlot,
    RecordPointer,
    ExceptionCode,
    ExceptionFlags
  } Field = Kind::PointersSlot;
  va_t Address = InvalidVA;
  int OpSeq = -1;
  va_t FilterVA = InvalidVA;
  bool operator==(const RegistrationSEHExceptionAccess &) const = default;
};

/// One read of the current PE32 NT_TIB. Self retains its exact source LOAD;
/// StackBase is an opaque runtime value, never a regenerated frame coordinate.
struct RegistrationSEHThreadAccess {
  static constexpr uint32_t WordBytes = 4;
  static constexpr uint32_t SelfOffset = 0x18;
  static constexpr uint32_t StackBaseOffset = 4;
  /// The lifter zero-extends PE32 effective addresses into its VA carrier.
  static constexpr bool isAddressWidth(uint16_t Bytes) {
    return Bytes == WordBytes || Bytes == sizeof(va_t);
  }
  enum class Kind : uint8_t { Self, StackBase } Field = Kind::Self;
  va_t Address = InvalidVA;
  int OpSeq = -1;
  va_t RootAddress = InvalidVA;
  int RootOpSeq = -1;
  bool operator==(const RegistrationSEHThreadAccess &) const = default;
};

/// A physical SEH finally invocation ends either by consuming its own near
/// return PC or by restoring the checked parent SavedESP and branching to an
/// exact parent instruction. CallVA is zero for a runtime invocation. These
/// facts authorize state/CFG flow, not native replacement of the invocation.
struct RegistrationSEHInvocationExit {
  enum class Kind : uint8_t {
    NearReturn,
    ParentJump,
    ParentFallthrough
  } Exit = Kind::NearReturn;
  va_t EntryVA = InvalidVA;
  va_t CallVA = 0;
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  va_t TargetVA = InvalidVA;
  va_t StackRestoreVA = InvalidVA;
  int StackRestoreSeq = -1;
  int32_t StackOffset = 0;
  bool isParentExit() const {
    return Exit == Kind::ParentJump || Exit == Kind::ParentFallthrough;
  }
  bool operator==(const RegistrationSEHInvocationExit &) const = default;
};

/// Exact source invocation of the EH3/EH4 capture protocol. Native
/// reconstruction must replace its physical register/PC contents and CRT state
/// arguments; copying the original jump buffer is not a continuation contract.
struct RegistrationSEHJumpCapture {
  enum class Kind : uint8_t { EH3, EH4 };
  static constexpr uint16_t BufferBytes = 64;
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  va_t TargetVA = InvalidVA;
  va_t UnwindVA = InvalidVA;
  int32_t BufferOffset = 0;
  int32_t StackOffset = 0;
  /// Sorted unique source states independently proved at this occurrence.
  std::vector<int32_t> Levels{-1};
  Kind Protocol = Kind::EH3;
  /// EH4's third unwind argument names the image's load-config cookie.
  /// It is an address, never the runtime value or an invented replacement.
  va_t SecurityCookieVA = 0;
  bool operator==(const RegistrationSEHJumpCapture &) const = default;
};

/// The logical state left by the last callback (or at unwind entry) and the
/// final runtime state after any remaining filter-only unwind steps.
struct RegistrationSEHJumpState {
  int32_t FrameLevel = -1;
  int32_t ResumeLevel = -1;
  /// The unwind bound saved by this invocation of the source capture.
  int32_t CaptureLevel = -1;
  auto operator<=>(const RegistrationSEHJumpState &) const = default;
};

/// A checked callback jump which abandons a pending jump for another live
/// capture. Bind the exact source occurrence and callback owner, never only
/// the runtime helper's address or the replacement capture's destination.
struct RegistrationSEHJumpReplacement {
  va_t Address = InvalidVA;
  int OpSeq = -1;
  va_t EntryVA = InvalidVA;
  va_t CaptureCall = InvalidVA;
  auto operator<=>(const RegistrationSEHJumpReplacement &) const = default;
};

/// A live SEH buffer supplies one physical capture PC and restored ESP. The
/// source invocation is retired by the authenticated nonlocal runtime call.
/// Unwind may restore the capture, select a handler or transfer to another
/// capture. None is an ordinary CALL return or a target-wide noreturn.
struct RegistrationSEHJumpResume {
  va_t Address = InvalidVA;
  int OpSeq = -1;
  va_t CaptureCall = InvalidVA;
  va_t TargetVA = InvalidVA;
  va_t EntryVA = InvalidVA;
  va_t OrdinaryCall = 0;
  int32_t BufferOffset = 0;
  int32_t StackOffset = 0;
  /// Cross-function exits bind the caller CALL above and the callee's checked
  /// runtime resume occurrence. These do not grant a callable native ABI.
  va_t CalleeVA = 0;
  va_t ResumeVA = 0;
  /// Exact resume-value argument slot: established EBP-relative for parent
  /// calls, or relative to EntryVA's private callback entry ESP. Cross-function
  /// callees require a separate stack owner and do not publish this projection.
  std::optional<int32_t> ValueOffset;
  /// Actual source states after local unwind. A capture inside a retired
  /// scope does not reactivate that scope when the continuation resumes.
  std::vector<int32_t> ResumedLevels;
  /// Sorted unique observations from every completed unwind path. Native
  /// resumption requires a single final state for each captured bound and
  /// observable logical frame state.
  std::vector<RegistrationSEHJumpState> StateRestores;
  /// Sorted unique source scopes selected by an exception raised during
  /// unwind. These paths enter their handlers and abandon this capture;
  /// they never contribute a restored register or state projection.
  std::vector<uint32_t> SelectedScopes;
  /// Sorted unique callback transfers which resume a different capture.
  /// Their frame and register effects never become this capture's outcomes.
  std::vector<RegistrationSEHJumpReplacement> Replacements;
  bool operator==(const RegistrationSEHJumpResume &) const = default;
};

/// One physical stack argument of an invocation-specialized SEH helper.
/// Scalar slots and borrowed objects use the caller's established EBP;
/// opaque buffers name a capture whose native allocation replaces the source.
struct RegistrationSEHBoundArgument {
  enum class Kind : uint8_t {
    Unused,
    Scalar,
    Object,
    JumpBuffer,
    NullPointer,
    ConstantScalar,
    ImagePointer
  };
  Kind ArgumentKind = Kind::Unused;
  int32_t Offset = 0;
  va_t Capture = 0;
  /// An invocation requirement emitted and checked as an actual i32 value.
  /// A frame-slot location alone cannot establish this constant.
  uint32_t ScalarValue = 0;
  /// Original mapped storage, distinct from a scalar with equal address bits.
  va_t ImageVA = 0;
  bool operator==(const RegistrationSEHBoundArgument &) const = default;
};

/// Invocation-bound native arguments. Preserved helpers require a conditional
/// frame-observation proof; imported runtimes require an authenticated import
/// and a complete invocation-specific memory and callback contract.
struct RegistrationSEHBoundCall {
  enum class Kind : uint8_t {
    PreservedHelper,
    RaiseException,
    ControlFP,
    GetStartupInfoW,
    HeapTermination,
    InterlockedCompareExchange,
    Sleep
  };
  /// SDK PE32 STARTUPINFOW extent, including the conditional standard handles.
  static constexpr uint32_t StartupInfoBytes = 68;
  /// HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0).
  static constexpr std::array<uint32_t, 4> HeapTerminationArguments = {0, 1, 0,
                                                                       0};
  Kind CalleeKind = Kind::PreservedHelper;
  va_t Address = InvalidVA;
  int OpSeq = -1;
  va_t TargetVA = InvalidVA;
  int32_t StackOffset = 0;
  /// Zero for the parent frame; otherwise the private callback stack owner.
  va_t EntryVA = 0;
  bool HasOrdinaryReturn = true;
  /// One layout-independent scalar on every ordinary helper return. This
  /// permits an exact EAX observation; it never replaces the native call.
  std::optional<uint32_t> ConstantReturn;
  std::vector<RegistrationSEHBoundArgument> Arguments;
  /// Native argument storage minimum. Preserved helpers additionally require
  /// their independently checked access extents.
  uint32_t borrowedObjectBytes() const {
    return CalleeKind == Kind::GetStartupInfoW ? StartupInfoBytes : 4;
  }
  bool operator==(const RegistrationSEHBoundCall &) const = default;
};

/// One installed SEH TryLevel store, bound to its decoded source operation.
/// Transitions preserve the whole state before and after a byte, word or dword
/// write; consumers do not infer untouched bytes from the encoded payload.
struct RegistrationSEHStateStore {
  int BlockId = -1;
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  int OpSeq = -1;
  uint8_t Width = 0;
  uint32_t Value = 0;
  std::vector<std::pair<int32_t, int32_t>> Transitions;
  bool operator==(const RegistrationSEHStateStore &) const = default;
};

/// A checked physical shared-prologue invocation. This is separate from an
/// ordinary callee ABI: it establishes EBP, allocates the node, preserves five
/// GPRs and publishes FS:[0]. Native reconstruction requires its own transfer.
struct RegistrationCxxPrologueEffect {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  va_t HelperVA = InvalidVA;
  int OpSeq = -1;
  int32_t StackOffset = 0;
  bool operator==(const RegistrationCxxPrologueEffect &) const = default;
};

/// The corresponding shared SEH4 source call. Its encoded scope table and
/// moved PC require a separate physical projection before native emission.
struct RegistrationSEH4PrologueEffect {
  va_t Address = InvalidVA;
  va_t EndAddress = InvalidVA;
  va_t HelperVA = InvalidVA;
  int OpSeq = -1;
  int32_t StackOffset = 0;
  bool operator==(const RegistrationSEH4PrologueEffect &) const = default;
};

struct RegistrationStateAnalysis {
  std::vector<RegistrationBlockState> Blocks;
  bool Complete = false;
  bool CallbackStatesComplete = true;
  bool RegistrationLifetimeComplete = false;
  bool CxxContinuationsComplete = true;
  std::vector<RegistrationCxxContinuation> CxxContinuations;
  bool SEHInvocationExitsComplete = true;
  /// Direct parent exits do not expose a stale CRT unwind guard to another
  /// call or faulting access. Logical source-state analysis can remain complete
  /// without this proof, but native parent-context reconstruction cannot.
  bool SEHParentChainComplete = true;
  std::vector<RegistrationSEHControlContract> SEHControls;
  std::vector<RegistrationSEHInvocationExit> SEHInvocationExits;
  bool SEHStateStoresComplete = true;
  std::vector<RegistrationSEHStateStore> SEHStateStores;
  bool SEHExceptionAccessesComplete = true;
  std::vector<RegistrationSEHExceptionAccess> SEHExceptionAccesses;
  bool SEHThreadAccessesComplete = true;
  std::vector<RegistrationSEHThreadAccess> SEHThreadAccesses;
  /// Invocation-specialized original helpers require a separate native plan.
  bool HasBoundSEHCallees = false;
  bool SEHBoundCallsComplete = true;
  std::vector<RegistrationSEHBoundCall> SEHBoundCalls;
  bool HasSEHLocalUnwinds = false;
  bool SEHLocalUnwindsComplete = true;
  std::vector<RegistrationSEHLocalUnwind> SEHLocalUnwinds;
  /// Original SEH captures retain their physical buffer and invocation. Native
  /// reconstruction requires a separate replacement-buffer contract.
  bool HasSEHJumpCaptures = false;
  bool SEHJumpCapturesComplete = true;
  std::vector<RegistrationSEHJumpCapture> SEHJumpCaptures;
  bool SEHJumpResumptionsComplete = true;
  std::vector<RegistrationSEHJumpResume> SEHJumpResumptions;
  /// Complete chain access ownership, including decoded boundaries and
  /// operation identities, available only with a complete registration
  /// lifetime and callback state proof. This is not a native output receipt.
  bool ChainOperationsComplete = false;
  std::vector<RegistrationChainAccess> ChainAccesses;
  std::optional<RegistrationCxxPrologueEffect> CxxPrologue;
  std::optional<RegistrationSEH4PrologueEffect> SEH4Prologue;
  std::vector<RegistrationFrameValue> FrameValues;
  /// The proven return cleanup of the callees this function calls. A
  /// callee's stack adjustment does not depend on the reaching registration
  /// state, so these remain when that proof is incomplete.
  std::vector<RegistrationCalleeStackContract> CalleeStacks;
  bool IncomingFrameAccessesComplete = true;
  std::vector<RegistrationIncomingFrameAccess> IncomingFrameAccesses;
  /// Every reaching call has checked source effects, possibly under an
  /// invocation-specific binding that grants no native call ABI.
  bool SourceCallFrameEffectsComplete = false;
  /// Per-occurrence stack and initialized-object projections with complete
  /// native call receipts. Source-only invocations cannot set this flag.
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
  /// EH4 source encoding or authenticated C++ handler cookie conditions have
  /// been checked across ordinary and runtime paths. This grants no physical
  /// output-cookie layout or permission to erase an observable encoded value.
  bool SecurityCookiesComplete = false;
  va_t SecurityCookieVA = 0;
  va_t CookieCheckVA = 0;
  std::vector<RegistrationCookieCheck> CookieChecks;
  std::vector<RegistrationCxxCookieCondition> CxxCookieConditions;
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
/// CxxHandler and CxxPrologue, when supplied, must be the loader's checked
/// descriptions for this source function and image; their conditions alone
/// do not prove the parent.
RegistrationStateAnalysis analyzeRegistrationStates(
    const LowFunc &Function, va_t SecurityCookieVA = 0, va_t CookieCheckVA = 0,
    const std::vector<RegistrationCalleeFrameContract> *Callees = nullptr,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups = nullptr,
    const std::vector<RegistrationCalleeStackContract> *Stacks = nullptr,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds = nullptr,
    const std::vector<RegistrationSEHControlContract> *SEHControls = nullptr,
    const coff_loader::X86CxxHandlerABI *CxxHandler = nullptr,
    const coff_loader::X86CxxPrologueCall *CxxPrologue = nullptr);

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
    if (Block.levels().empty())
      continue;
    const bool Included = Contains(Block.levels().front());
    for (int32_t Level : Block.levels())
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
