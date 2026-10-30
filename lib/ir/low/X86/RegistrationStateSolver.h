//===- RegistrationStateSolver.h - x86 EH state solver --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Solve registration lifetimes, caller-frame ownership and invocation effects.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H

#include "RegistrationConditionalFrame.h"
#include "RegistrationFrame.h"
#include "RegistrationJumpCapture.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"

#include <compare>
#include <map>
#include <set>
#include <tuple>

namespace neverd::coff_loader {
struct X86CxxEpilogueCall;
struct X86SEH4PrologueCall;
struct X86SEH4EpilogueCall;
} // namespace neverd::coff_loader

namespace neverd::registration_state {

struct CxxCatchContext {
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  /// The CRT captures SavedESP before entering a catch and restores that
  /// snapshot on return or guard unwind, even if the catch rewrites the slot.
  std::optional<int32_t> SavedStackOffset;
  std::optional<CallbackFrameAddress> SavedCallback;
  std::optional<va_t> SuspendedCallback;
  std::optional<int32_t> SuspendedStackOffset;
  std::map<int32_t, FrameValue> SuspendedCells;
  std::set<int32_t> SuspendedInitializedBytes;
  size_t cellCount() const {
    return 1 + SuspendedCells.size() + SuspendedInitializedBytes.size();
  }
  FrameValue savedStack() const {
    if (SavedCallback)
      return FrameValue::callbackFrame(SavedCallback->Entry,
                                       SavedCallback->Offset);
    return SavedStackOffset ? FrameValue::frame(*SavedStackOffset)
                            : FrameValue{{}, {}, false, true};
  }
  auto operator<=>(const CxxCatchContext &) const = default;
};

/// SEH filters and finally handlers own distinct runtime invocations even
/// when their decoded bodies share an epilogue. Searching and unwinding also
/// have different dispatch rules. The default key retains the existing C++
/// catch-stack domain and ordinary parent flow.
struct SEHActivation {
  std::optional<va_t> Entry;
  bool SearchFilter = false;
  va_t OrdinaryCall = 0;
  /// Keep pre-capture callbacks separate from callbacks whose parent owns a
  /// checked capture. Joining these loses the buffer/guard correlation.
  std::vector<std::pair<int32_t, va_t>> JumpCaptureCalls;
  std::vector<std::pair<va_t, va_t>> Callers;
  auto operator<=>(const SEHActivation &) const = default;
};

/// Keep C++ cleanup ownership and a live SEH buffer's saved unwind bound
/// correlated with the reaching state. Other facts still join at the same
/// state; this key grants no initialization or value authority of its own.
struct SEHFlowKey {
  SEHActivation Activation;
  std::vector<int32_t> Levels;
  std::vector<std::tuple<int32_t, va_t, int32_t>> Captures;
  auto operator<=>(const SEHFlowKey &) const = default;
};

struct Domain {
  SEHActivation Activation;
  std::set<int32_t> Levels;
  FrameState Frame;
  FrameState RuntimeObject;
  std::set<int32_t> InitializedFrameBytes;
  bool Reached = false;
  bool Unknown = false;
  bool Parent = false;
  bool Callback = false;
  bool OtherCallback = false;
  using CatchStack = std::vector<CxxCatchContext>;
  std::set<CatchStack> CxxCatchStacks;
  bool Uninstalled = false;
  bool Installed = false;
  /// A runtime finally left its CRT guard without returning through the CRT.
  /// The parent must reconnect its own node or remove the chain before calls.
  bool AbandonedSEHGuard = false;
  bool CanDispatch = false;
  /// All possible exception origins so far have closed integer effects and
  /// closed C++ callbacks. Distinct from ordinary-return IAT liveness.
  bool CxxDispatchBindingsIntact = true;
  std::optional<int32_t> OrdinaryCallStack;
  struct ParentStackRestore {
    va_t Address = InvalidVA;
    int Seq = -1;
    int32_t StackOffset = 0;
    bool operator==(const ParentStackRestore &) const = default;
  };
  std::optional<ParentStackRestore> StackRestore;
  std::map<int32_t, SEHJumpCapture> JumpCaptures;
  struct SEHCaller {
    SEHActivation Activation;
    FrameState Stack;
    bool OtherCallback = false;
    std::optional<int32_t> OrdinaryCallStack;
    std::optional<ParentStackRestore> StackRestore;
    bool operator==(const SEHCaller &) const = default;
  };
  std::vector<SEHCaller> SEHCallers;
  size_t sehCallerCellCount() const {
    size_t Count = 0;
    for (const auto &Caller : SEHCallers)
      Count += Caller.Stack.cellCount() +
               Caller.Stack.OtherRegisterBytes.size() +
               Caller.Activation.JumpCaptureCalls.size() +
               Caller.Activation.Callers.size() + 8;
    return Count;
  }
  bool JumpBindingsIntact = true;
  size_t catchCellCount() const {
    size_t Count = 0;
    for (const auto &Stack : CxxCatchStacks)
      for (const auto &Context : Stack)
        Count += Context.cellCount();
    return Count;
  }
  size_t snapshotWork() const {
    return Levels.size() + Frame.cellCount() + Frame.OtherRegisterBytes.size() +
           RuntimeObject.cellCount() + RuntimeObject.OtherRegisterBytes.size() +
           InitializedFrameBytes.size() + JumpCaptures.size() * 14 +
           Activation.JumpCaptureCalls.size() + Activation.Callers.size() +
           sehCallerCellCount() + catchCellCount() + 10;
  }
  /// A lookup accelerator; complete equality remains the identity check.
  size_t snapshotHash() const;
  bool operator==(const Domain &) const = default;
};

/// Facts inherited by a runtime dispatch. Entry construction replaces the
/// registers, invocation stack and activation; captures retain their own saved
/// registers. Both root construction and pending-root identity use this owner.
Domain dispatchSource(const Domain &Source);

/// Frames with different native TryLevels must not lose their correlation.
using SEHFrameOutcomes = std::map<int32_t, FrameState>;

/// The filter disposition chooses whether the captured search cursor resumes.
/// Keep it paired with the frame effects and the callback-written TryLevel.
using SEHFilterOutcomes = std::map<uint32_t, SEHFrameOutcomes>;

/// A selected scope retains its captured search cursor separately from the
/// filter-written state which controls the following local unwind.
using SEHHandlerOutcomes = std::map<uint32_t, SEHFrameOutcomes>;

/// A replacement jump can change EAX without changing its captured buffer.
/// Pair each value with the frame effects which actually resume with it.
using SEHJumpFrameOutcomes = std::map<FrameValue, SEHFrameOutcomes>;

struct SEHUnwindJumpValue {
  int32_t BufferOffset;
  FrameValue Value;
  RegistrationSEHJumpReplacement Source;
  auto operator<=>(const SEHUnwindJumpValue &) const = default;
};

// A repeated jump retires the pending handler and every suspended callback.
// Its frames must not be projected through an ordinary callback return.
struct SEHUnwindJump {
  const std::map<int32_t, SEHJumpCapture> &Captures;
  std::map<SEHUnwindJumpValue, SEHFrameOutcomes> Resumed;
};

/// Each destination keeps its own saved registers, stack and unwind bound.
/// Only paths restoring Capture contribute its state projection or levels.
struct SEHJumpUnwind {
  const SEHJumpCapture &Capture;
  const FrameValue &Value;
  const std::map<int32_t, SEHJumpCapture> &Captures;
  std::map<int32_t, SEHJumpFrameOutcomes> Resumed;
  std::set<RegistrationSEHJumpState> StateRestores;
  std::set<RegistrationSEHJumpReplacement> Replacements;
};

struct BlockFacts {
  bool Invalid = false;
  bool InstalledAtExit = false;
  bool CxxContinuationAtExit = false;
  bool NoReturnAtExit = false;
  bool InvocationAtExit = false;
  bool CallbackReturnAtExit = false;
};

struct ActivationState {
  Domain Incoming;
  BlockFacts Facts;
};

int32_t cxxMinimumTryLevel(const Domain &State, const CxxExceptionInfo &Cxx);
size_t retainedCxxCatchDepth(const Domain::CatchStack &Stack,
                             const CxxExceptionInfo &Cxx, uint32_t TryIndex);

/// One bounded fixed-point analysis. Ordinary transfers and exceptional roots
/// share the same frame lattice, source occurrences and cumulative work limit.
class RegistrationStateSolver {
public:
  RegistrationStateSolver(
      const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
      const std::vector<RegistrationCalleeFrameContract> *Callees,
      const std::vector<RegistrationCleanupFrameContract> *Cleanups,
      const std::vector<RegistrationCalleeStackContract> *Stacks,
      const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
      const std::vector<RegistrationSEHControlContract> *SEHControls,
      ConditionalFrame *Conditional = nullptr,
      const BoundCalleeProver *BoundCallees = nullptr,
      const SEHJumpRuntime *JumpRuntime = nullptr,
      const coff_loader::X86CxxHandlerABI *CxxHandler = nullptr,
      const coff_loader::X86CxxPrologueCall *CxxPrologue = nullptr,
      const std::vector<coff_loader::X86CxxEpilogueCall> *CxxEpilogues =
          nullptr,
      const coff_loader::X86SEH4PrologueCall *SEH4Prologue = nullptr,
      const std::vector<coff_loader::X86SEH4EpilogueCall> *SEH4Epilogues =
          nullptr,
      const CxxCallerProof *CxxCalls = nullptr);
  RegistrationStateAnalysis run(size_t InitialWork = 0);
  size_t workUsed() const { return WorkUsed; }

private:
  bool initialize();
  bool initializeOrder();
  bool extendSEHParentOwnership();
  bool validateRealignedLayout();
  bool realignedMemoryIsDisjoint(const FrameValue &Address,
                                 uint16_t Width) const;
  bool registrationInstallationReady(const FrameState &Frame) const;
  bool validateCxxPrologue() const;
  bool validateSEH4Prologue() const;
  bool transferSEH4Prologue(size_t I, Domain &State, const LowOp &Op,
                            BlockFacts &Facts);
  bool transferSEH4Epilogue(size_t I, Domain &State, const LowOp &Op,
                            BlockFacts &Facts,
                            const coff_loader::X86SEH4EpilogueCall &Call);
  bool transferCxxPrologue(size_t I, Domain &State, const LowOp &Op,
                           BlockFacts &Facts);
  bool transferCxxEpilogue(size_t I, Domain &State, const LowOp &Op,
                           BlockFacts &Facts,
                           const coff_loader::X86CxxEpilogueCall &Call);
  bool validateSEHParentChainUse(const Domain &State, const LowOp &Op,
                                 const FrameTransfer &Transfer) const;
  bool canReconnectSEHParentChain(const Domain &State) const;
  bool callbackCanReturn(const Domain &State) const;
  bool callbackReturnInstruction(size_t I, const LowOp &Op) const;
  std::optional<int32_t> parentStackOffset(const Domain &State) const;
  void preserveCxxFrameCells(Domain &Root, const Domain &Source);
  bool initializeContracts();
  std::optional<FrameValue> transferPrivateSEHCall(Domain &State,
                                                   const LowOp &Op,
                                                   const BlockFacts &Facts);
  bool bindRuntimeObjectBorrow(const Domain &State,
                               const RegistrationCalleeFrameContract &Callee,
                               RegistrationCallFrameEffect &Effect);
  std::optional<RegistrationRuntimeThrow>
  runtimeThrowArguments(const Domain &State,
                        const RegistrationCalleeFrameContract &Callee,
                        std::vector<RegistrationObjectExtent> &Reads);
  void initializeCookies();
  bool hasCompleteCookieChecks();
  void collectOccurrences();
  void seedEntries();
  struct CallTransfer {
    FrameValue StackPointer;
    bool DoesNotReturn;
    va_t EndAddress;
    std::optional<FrameValue> Result;
    /// Invocation-bound writes may also precede exceptional dispatch.
    std::vector<RegistrationObjectExtent> MayWrites;
    /// Complete image may-writes have already invalidated affected bindings.
    /// This does not authorize retaining cached callable register identities.
    bool PreservesImportBindings = false;
    bool PreservesDispatchBindings = false;
  };
  std::optional<CallTransfer> transferCall(size_t I, Domain &After,
                                           const LowOp &Op,
                                           const BlockFacts &Facts);
  std::optional<CallTransfer>
  transferConditionalCxxCall(Domain &After, const LowOp &Op,
                             const BlockFacts &Facts);
  bool transferScalarArguments(Domain &After,
                               const RegistrationCalleeFrameContract &Callee,
                               FrameValue &StackPointer,
                               std::optional<int32_t> Object);
  bool transferInitializer(Domain &After,
                           const RegistrationCalleeFrameContract &Callee,
                           FrameValue &StackPointer, FrameValue &Object);
  std::optional<FrameValue> transferLocalUnwind(size_t I, Domain &After,
                                                const LowOp &Op,
                                                BlockFacts &Facts);
  void recordLocalUnwind(const Domain &State, RegistrationSEHLocalUnwind Call);
  bool transferOrdinaryFinally(size_t I, Domain &After, const LowOp &Op,
                               BlockFacts &Facts,
                               std::optional<FrameValue> &ReturnedSP);
  bool enterOrdinaryFinally(size_t I, const Domain &Before, const LowOp &Op,
                            BlockFacts &Facts);
  std::optional<va_t> sehCallContinuation(va_t Call, va_t CallerEntry = 0);
  bool validateSEHCallContinuation(size_t I, const LowOp &Op,
                                   va_t CallerEntry = 0);
  bool validateSEHInvocationBlock(size_t I);
  std::optional<va_t> knownSuccessor(const LowBlock &Block,
                                     const FrameTransfer &Transfer) const;
  bool transferSEHExit(size_t I, Domain &After, const LowOp &Op,
                       BlockFacts &Facts);
  bool transferSEHParentFallthrough(size_t I, Domain &After, const LowOp &Op,
                                    BlockFacts &Facts);
  bool proveSEHParentEpilogue(size_t I, const FrameState &Frame);
  bool returnNestedFinally(Domain &After, const LowOp &Op, BlockFacts &Facts,
                           RegistrationSEHInvocationExit Exit);
  void recordSEHStackRestore(const LowOp &Op, Domain &After,
                             const FrameTransfer &Transfer,
                             const FrameValue &Value);
  bool transferTryLevelStore(size_t I, const LowOp &Op,
                             const FrameValue &Address,
                             const FrameValue &Stored,
                             const std::set<int32_t> &Levels, bool Unknown,
                             std::set<int32_t> &WrittenLevels);
  bool mergeSEHFrameOutcome(SEHFrameOutcomes &Outcomes, FrameState State);
  bool runSEHUnwind(
      FrameState Frame, int32_t Level, uint32_t Bound,
      SEHFrameOutcomes &Outcomes, std::vector<RegistrationObjectExtent> &Writes,
      SEHJumpUnwind *Jump = nullptr, SEHHandlerOutcomes *Selected = nullptr,
      std::set<std::pair<int32_t, int32_t>> *Transitions = nullptr);
  bool runLocalFinally(va_t Target, FrameState &Parent,
                       bool OrdinaryInvocation = false,
                       std::vector<RegistrationObjectExtent> *Writes = nullptr,
                       SEHUnwindJump *Jump = nullptr,
                       SEHFrameOutcomes *Outcomes = nullptr,
                       SEHHandlerOutcomes *Selected = nullptr);
  bool
  runSEHCallbackFrame(va_t Target, FrameState &Parent, bool OrdinaryInvocation,
                      std::vector<RegistrationObjectExtent> *Writes,
                      SEHUnwindJump *Jump, SEHFilterOutcomes *FilterOutcomes,
                      unsigned Depth = 0,
                      std::optional<int32_t> ParentStackOffset = std::nullopt,
                      SEHFrameOutcomes *Outcomes = nullptr,
                      SEHHandlerOutcomes *Selected = nullptr);
  bool runSEHNestedFinallyFrame(size_t I, const LowOp &Op, FrameState &State,
                                int32_t ParentStackOffset,
                                std::vector<RegistrationObjectExtent> *Writes,
                                unsigned Depth, SEHUnwindJump *Jump,
                                SEHFrameOutcomes &Outcomes,
                                SEHHandlerOutcomes *Selected);
  bool recordSEHUnwindJump(size_t I, const LowOp &Op, const FrameState &State,
                           va_t Callback, SEHUnwindJump &Jump);
  bool runSEHFilterSearch(const FrameState &Parent,
                          std::vector<RegistrationObjectExtent> *Writes,
                          SEHFrameOutcomes &Outcomes, bool RequireResume,
                          SEHHandlerOutcomes *Selected = nullptr,
                          SEHUnwindJump *Jump = nullptr);
  bool mergeSEHJumpContinuation(const Domain &Source,
                                const SEHJumpCapture &Saved,
                                const FrameValue &Value,
                                SEHFrameOutcomes Outcomes);
  bool applySEHContinueFilters(size_t I, Domain &State,
                               const BlockFacts &Facts);
  void
  recordCatchReturn(size_t I, const Domain &After, const LowOp &Op,
                    const FrameTransfer &Transfer, const BlockFacts &Facts,
                    std::optional<RegistrationCxxContinuation> &CatchReturn);
  void resumeCxxCatch(const Domain &After,
                      const RegistrationCxxContinuation &Return);
  bool recordRuntimeMemory(size_t I, Domain &After, const LowOp &Op,
                           const FrameTransfer &Transfer,
                           const FrameTransfer &RuntimeTransfer,
                           const BlockFacts &Facts);
  bool recordSEHExceptionMemory(size_t I, Domain &After, const LowOp &Op,
                                const FrameTransfer &Transfer,
                                BlockFacts &Facts);
  bool recordSEHThreadMemory(size_t I, Domain &After, const LowOp &Op,
                             const FrameTransfer &Transfer, BlockFacts &Facts);
  bool runtimeObjectIsLive(const Domain &State,
                           std::pair<uint32_t, uint32_t> Identity);
  bool transferBlock(size_t I, const Domain &Before, BlockFacts &Facts);
  bool transferBoundSEHCallee(size_t I, const Domain &Before, Domain &After,
                              const LowOp &Op, BlockFacts &Facts,
                              std::optional<FrameValue> &CallSP,
                              std::optional<Domain> &CallException,
                              std::optional<FrameValue> &CallResult);
  bool transferJumpCapture(size_t I, Domain &After, const LowOp &Op,
                           const BlockFacts &Facts,
                           std::optional<FrameValue> &CallSP);
  bool transferJumpResume(size_t I, Domain &After, const LowOp &Op,
                          BlockFacts &Facts);
  bool transferBorrowedJump(size_t I, Domain &After, const LowOp &Op,
                            BlockFacts &Facts, const FrameValue &Buffer,
                            const FrameValue &Value);
  std::optional<FrameValue>
  transferControlFP(const Domain &Before, Domain &After, const LowOp &Op,
                    const BlockFacts &Facts,
                    std::optional<Domain> &CallException);
  std::optional<FrameValue>
  transferStartupInfo(const Domain &Before, Domain &After, const LowOp &Op,
                      const BlockFacts &Facts,
                      std::optional<Domain> &CallException);
  std::optional<FrameValue> transferHeapTermination(const Domain &State,
                                                    const LowOp &Op,
                                                    const BlockFacts &Facts);
  std::optional<FrameValue>
  transferSynchronization(const Domain &State, const LowOp &Op,
                          const BlockFacts &Facts,
                          std::optional<FrameValue> &CallResult);
  void recordSEHRuntimeCall(const Domain &State, const LowOp &Op,
                            RegistrationSEHBoundCall::Kind Kind,
                            std::optional<int32_t> Output = std::nullopt,
                            va_t ImageOutput = 0);
  bool resumeJumpCapture(Domain &After, BlockFacts &Facts, const LowOp &Op,
                         const SEHJumpCapture &Saved, const FrameValue &Value,
                         va_t CalleeVA = 0, va_t ResumeVA = 0);
  void invalidateJumpCaptures(Domain &State, const FrameValue &Address,
                              uint16_t Width);
  void invalidateJumpRuntimeBindings(Domain &State, const FrameValue &Address,
                                     uint64_t Width);
  bool checkConditionalMemory(const FrameState &State, const LowOp &Op,
                              const FrameTransfer &Transfer,
                              const std::set<int32_t> *Initialized = nullptr);
  bool conditionalReturn(const Domain &State, const LowOp &Op);
  bool conditionalCall(const Domain &State, const LowOp &Op,
                       std::optional<FrameValue> &ReturnValue);
  bool preservesCxxDispatchBindings(const Domain &State, const LowOp &Op,
                                    const FrameTransfer &Transfer,
                                    bool RuntimeMemory);
  void dispatchBlock(size_t I, const Domain &Before, const BlockFacts &Facts,
                     bool PreserveBindings = false);
  bool dispatchUnknownCxxCleanups(const Domain &Before);
  void queueSEHScopes(const Domain &Before, bool Invalid);
  void flushSEHScopes();
  void dispatchSEHScopes(const Domain &Before, bool Invalid,
                         const std::set<int32_t> &Levels);
  std::vector<RegistrationCxxSearch> cxxSearches(const Domain &State,
                                                 bool Invalid);
  bool enterCxxCatch(Domain &Root, const Domain &Source, uint32_t TryIndex,
                     uint32_t CatchIndex);
  RegistrationStateAnalysis finish();

  bool charge(size_t Amount);
  bool validLevel(int32_t Level) const { return AllLevels.count(Level) != 0; }
  bool validObjects(llvm::ArrayRef<RegistrationObjectExtent> Ranges);
  bool validImageRanges(llvm::ArrayRef<ExceptionAddressRange> Ranges);
  /// Source object borrows and runtime construction cannot own registration,
  /// saved-stack, exception-pointer, realigned-anchor or cookie storage.
  bool frameObjectIsDisjointFromAdministration(int64_t Begin,
                                               int64_t End) const;
  std::optional<RegistrationObjectExtent>
  cxxBorrowedStorage(const Domain &State, int32_t Begin);
  bool projectFrameObject(const Domain &State, int32_t Object,
                          std::optional<int32_t> SP,
                          llvm::ArrayRef<RegistrationObjectExtent> Extents,
                          std::vector<RegistrationObjectExtent> &Projected,
                          bool Reads, bool RecordBorrowedEffects = true,
                          bool RequireInitializedReads = true);
  std::optional<int32_t> cookieSlot(int32_t Displacement) const;
  bool cookiesReady(const FrameState &Frame) const;
  bool cxxCookiesReady(const Domain &State) const;
  void initializeCxxCookies();
  void recordChainAccess(const LowOp &Op, const LowBlock &Block,
                         RegistrationChainAccess::Kind Kind);
  void merge(size_t Target, const Domain &Source);
  void
  dispatch(va_t Address, std::set<int32_t> Levels, const Domain &Source,
           bool Unknown = false, bool Callback = false,
           bool SearchFilter = false,
           std::optional<std::pair<uint32_t, uint32_t>> CxxCatch = std::nullopt,
           bool PreserveBindings = false);

  const LowFunc &Function;
  const ExceptionFunction &EH;
  const RegistrationChainInfo &Chain;
  va_t SecurityCookieVA;
  va_t CookieCheckVA;
  const coff_loader::X86CxxHandlerABI *CxxHandler;
  const coff_loader::X86CxxPrologueCall *CxxPrologue;
  const coff_loader::X86SEH4PrologueCall *SEH4Prologue;
  const std::vector<coff_loader::X86SEH4EpilogueCall> *SEH4Epilogues;
  std::map<va_t, const coff_loader::X86SEH4EpilogueCall *> SEH4EpilogueIndex;
  const std::vector<coff_loader::X86CxxEpilogueCall> *CxxEpilogues;
  std::map<va_t, const coff_loader::X86CxxEpilogueCall *> CxxEpilogueIndex;
  const std::vector<RegistrationCalleeFrameContract> *Callees;
  const std::vector<RegistrationCleanupFrameContract> *Cleanups;
  const std::vector<RegistrationCalleeStackContract> *Stacks;
  const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds;
  const std::vector<RegistrationSEHControlContract> *SEHControls;
  ConditionalFrame *Conditional;
  const BoundCalleeProver *BoundCallees;
  const SEHJumpRuntime *JumpRuntime;
  const CxxCallerProof *CxxCalls;
  const bool KnownCxx;
  const bool EH4;
  const bool CheckCalls;
  const bool TrackRuntimeArguments;
  const bool CheckRuntimeObjects;
  const bool CheckCleanups;
  RegistrationStateAnalysis Result;

  /// One entry seed owns both invocation refinements and opaque identities.
  /// Shared prologues must preserve this seed through their decoded prefix.
  decltype(FrameState::Registers) ParentEntryRegisters;

  std::map<int, size_t> Index;
  std::map<va_t, size_t> Entries;
  std::vector<bool> InvalidBlocks;
  std::map<va_t, RegistrationTryLevelStore> Stores;
  std::map<std::pair<va_t, int>, RegistrationSEHStateStore> SEHStateStores;
  std::set<int32_t> AllLevels;
  std::set<va_t> FinallyTargets;
  std::set<va_t> SEHParentEntries;
  /// Physically anchored tails become parent code only after a checked
  /// SavedESP transfer. They grant no runtime-handler entry memory privilege.
  std::set<va_t> SEHParentContinuations;
  std::vector<std::map<SEHFlowKey, ActivationState>> States;
  struct SEHDispatchMemo {
    Domain Source;
    bool Invalid;
    std::set<int32_t> Levels;
    std::set<int32_t> Pending;
  };
  std::vector<SEHDispatchMemo> DispatchedSEHFrames;
  std::map<size_t, std::vector<size_t>> SEHFrameBuckets;
  std::set<size_t> PendingSEHFrames;
  std::vector<size_t> Order;
  std::set<std::tuple<size_t, size_t, SEHFlowKey>> Work;
  size_t WorkUsed = 0;
  bool Exhausted = false;
  bool ProvenInstallation = false;
  std::optional<RegistrationCxxPrologueEffect> CxxPrologueEffect;
  std::optional<RegistrationSEH4PrologueEffect> SEH4PrologueEffect;
  bool CompleteChainOperations = true;
  std::map<std::pair<va_t, int>, RegistrationChainAccess> ChainAccesses;
  std::map<va_t, std::pair<int, LowInstructionBoundary>> Boundaries;
  std::set<std::pair<va_t, int>> ChainOccurrences;
  std::map<std::pair<va_t, int>, FrameValue> FrameValues;

  std::map<va_t, uint32_t> CalleeIndices;
  std::map<std::pair<va_t, bool>, uint32_t> StackPops;
  std::map<std::pair<va_t, int>, va_t> StackImportLoads;
  std::set<va_t> StackImportSlots;
  std::set<va_t> StackBindingCallees;
  std::set<va_t> StackImportVeneers;
  std::set<std::pair<va_t, bool>> StackBindingPreservers;
  std::map<std::pair<va_t, bool>, RegistrationLocalUnwindContract>
      LocalUnwindTargets;
  std::set<va_t> LocalUnwindImportSlots;
  std::map<std::pair<va_t, int>, RegistrationSEHLocalUnwind> LocalUnwindCalls;
  bool FailedLocalUnwind = false;
  std::map<va_t, RegistrationSEHControlContract> SEHControlIndex;
  using SEHExitKey = std::tuple<va_t, va_t, va_t, int>;
  std::map<SEHExitKey, RegistrationSEHInvocationExit> SEHExits;
  std::set<SEHExitKey> InvalidSEHExits;
  std::map<SEHExitKey, RegistrationSEHJumpResume> JumpResumptions;
  std::map<std::pair<va_t, int>, RegistrationSEHBoundCall> BoundCalls;
  /// A runtime occurrence must retain its binding on every reaching visit,
  /// including later loop iterations and distinct callback activations.
  std::set<std::pair<va_t, int>> InvalidRuntimeBindings;
  std::map<std::pair<va_t, int>, RegistrationSEHJumpCapture>
      JumpCaptureOccurrences;
  std::set<va_t> ModifiedCallbackCaptures;
  std::map<uint32_t, uint32_t> CleanupIndices;
  // Only immutable target/state pairs are shared across unknown dispatches.
  // Each source still supplies its own frame to every callback transfer.
  std::optional<std::vector<std::pair<va_t, int32_t>>> UnknownCxxCleanupRoots;
  bool CompleteCalls;
  bool HasSourceOnlyCallEffects = false;
  bool CompleteCleanups;
  bool CompleteCatchObjects;
  bool CompleteRuntimeObjects;
  bool CompleteCxxContinuations = true;
  std::map<std::pair<uint32_t, uint32_t>, RegistrationCxxCatchObject>
      CatchObjects;
  std::map<std::pair<va_t, int>, RegistrationCxxContinuation> CxxContinuations;
  std::set<std::pair<va_t, int>> InvalidCxxContinuations;
  std::map<std::pair<va_t, int>, RegistrationCallFrameEffect> CallEffects;
  std::set<std::pair<va_t, int>> InvalidCalls;
  std::map<std::pair<va_t, int>, RegistrationRuntimeObjectAccess>
      RuntimeAccesses;
  std::set<std::pair<va_t, int>> InvalidRuntimeAccesses;
  using CleanupKey = std::tuple<int, int32_t, uint32_t>;
  std::map<CleanupKey, RegistrationCleanupFrameEffect> CleanupEffects;
  std::set<CleanupKey> InvalidCleanups;

  bool ConsistentIncomingAccesses = true;
  bool CompleteImageReads = true;
  std::map<std::pair<va_t, int>, std::optional<RegistrationIncomingFrameAccess>>
      IncomingAccesses;
  std::set<std::pair<va_t, va_t>> ImageReads;
  std::map<std::pair<va_t, int>, std::optional<RegistrationSEHExceptionAccess>>
      SEHExceptionAccesses;
  std::map<std::pair<va_t, int>, std::optional<RegistrationSEHThreadAccess>>
      SEHThreadAccesses;

  bool CompleteCookies = false;
  bool CxxCookies = false;
  std::vector<RegistrationCxxCookieCondition> CxxCookieConditions;
  std::optional<int32_t> EHCookieSlot;
  std::optional<int32_t> GSCookieSlot;
  std::map<va_t, int> CookieCheckOccurrences;
  std::map<std::pair<va_t, int>, RegistrationCookieCheck> CookieChecks;
  bool ReadsGSCookie = false;
};

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H
