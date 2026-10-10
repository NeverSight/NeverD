//===- RegistrationStateSolver.h - x86 EH state solver --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H

#include "RegistrationFrame.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"

#include <compare>
#include <deque>
#include <map>
#include <set>
#include <tuple>

namespace neverd::registration_state {

struct CxxCatchContext {
  uint32_t TryIndex = 0;
  uint32_t CatchIndex = 0;
  /// The CRT captures SavedESP before entering a catch and restores that
  /// snapshot on return, even if the catch writes another value to the slot.
  std::optional<int32_t> SavedStackOffset;
  auto operator<=>(const CxxCatchContext &) const = default;
};

struct Domain {
  std::set<int32_t> Levels;
  FrameState Frame;
  FrameState RuntimeObject;
  std::optional<RegistrationCxxCatchObject> RuntimeIdentity;
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
  bool CanDispatch = false;
};

struct BlockFacts {
  bool Invalid = false;
  bool InstalledAtExit = false;
  bool CxxContinuationAtExit = false;
  bool NoReturnAtExit = false;
};

int32_t cxxMinimumTryLevel(const Domain &State, const CxxExceptionInfo &Cxx);

/// One bounded fixed-point analysis. Ordinary transfers and exceptional roots
/// share the same frame lattice, source occurrences and cumulative work limit.
class RegistrationStateSolver {
public:
  RegistrationStateSolver(
      const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
      const std::vector<RegistrationCalleeFrameContract> *Callees,
      const std::vector<RegistrationCleanupFrameContract> *Cleanups);
  RegistrationStateAnalysis run();

private:
  bool initialize();
  bool validateRealignedLayout();
  bool realignedMemoryIsDisjoint(const FrameValue &Address,
                                 uint16_t Width) const;
  bool registrationInstallationReady(const FrameState &Frame) const;
  bool callbackCanReturn(const Domain &State) const;
  std::optional<int32_t> parentStackOffset(const Domain &State) const;
  bool initializeContracts();
  void initializeCookies();
  void collectOccurrences();
  void seedEntries();
  struct CallTransfer {
    FrameValue StackPointer;
    bool DoesNotReturn;
    va_t EndAddress;
  };
  std::optional<CallTransfer> transferCall(size_t I, Domain &After,
                                           const LowOp &Op);
  void
  recordCatchReturn(size_t I, const Domain &After, const LowOp &Op,
                    const FrameTransfer &Transfer,
                    std::optional<RegistrationCxxContinuation> &CatchReturn);
  bool recordRuntimeMemory(size_t I, Domain &After, const LowOp &Op,
                           const FrameTransfer &Transfer,
                           const FrameTransfer &RuntimeTransfer);
  bool transferBlock(size_t I);
  void dispatchBlock(size_t I, const Domain &Before);
  RegistrationStateAnalysis finish();

  bool charge(size_t Amount);
  bool validLevel(int32_t Level) const { return AllLevels.count(Level) != 0; }
  bool validObjects(llvm::ArrayRef<RegistrationObjectExtent> Ranges);
  bool validImageRanges(llvm::ArrayRef<ExceptionAddressRange> Ranges);
  bool projectFrameObject(const Domain &State, int32_t Object,
                          std::optional<int32_t> SP,
                          llvm::ArrayRef<RegistrationObjectExtent> Extents,
                          std::vector<RegistrationObjectExtent> &Projected,
                          bool Reads);
  std::optional<int32_t> cookieSlot(int32_t Displacement) const;
  bool cookiesReady(const FrameState &Frame) const;
  void recordChainAccess(const LowOp &Op, const LowBlock &Block,
                         RegistrationChainAccess::Kind Kind);
  void merge(size_t Target, const Domain &Source);
  void dispatch(
      va_t Address, int32_t Level, const Domain &Source, bool Unknown = false,
      bool Callback = false, bool SearchFilter = false,
      std::optional<std::pair<uint32_t, uint32_t>> CxxCatch = std::nullopt);

  const LowFunc &Function;
  const ExceptionFunction &EH;
  const RegistrationChainInfo &Chain;
  va_t SecurityCookieVA;
  va_t CookieCheckVA;
  const std::vector<RegistrationCalleeFrameContract> *Callees;
  const std::vector<RegistrationCleanupFrameContract> *Cleanups;
  const bool KnownCxx;
  const bool EH4;
  const bool CheckCalls;
  const bool CheckRuntimeObjects;
  const bool CheckCleanups;
  RegistrationStateAnalysis Result;

  std::map<int, size_t> Index;
  std::map<va_t, size_t> Entries;
  std::vector<BlockFacts> Facts;
  std::map<va_t, RegistrationTryLevelStore> Stores;
  std::set<int32_t> AllLevels;
  std::vector<Domain> Incoming;
  std::deque<size_t> Work;
  std::vector<bool> Queued;
  size_t WorkUsed = 0;
  bool Exhausted = false;
  bool ProvenInstallation = false;
  bool CompleteChainOperations = true;
  std::map<std::pair<va_t, int>, RegistrationChainAccess> ChainAccesses;
  std::map<va_t, std::pair<int, LowInstructionBoundary>> Boundaries;
  std::set<std::pair<va_t, int>> ChainOccurrences;
  std::map<std::pair<va_t, int>, FrameValue> FrameValues;

  std::map<va_t, uint32_t> CalleeIndices;
  std::map<uint32_t, uint32_t> CleanupIndices;
  bool CompleteCalls;
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

  bool CompleteCookies = false;
  std::optional<int32_t> EHCookieSlot;
  std::optional<int32_t> GSCookieSlot;
  std::map<va_t, int> CookieCheckOccurrences;
  std::map<std::pair<va_t, int>, RegistrationCookieCheck> CookieChecks;
  bool ReadsGSCookie = false;
};

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSTATESOLVER_H
