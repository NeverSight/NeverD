//===- RegistrationState.cpp - x86 EH state analysis ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Solve source registration states with separately bound call authorities.
///
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"
#include "neverd/loader/RegistrationScopeGraph.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

RegistrationStateSolver::RegistrationStateSolver(
    const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls,
    ConditionalFrame *Conditional, const BoundCalleeProver *BoundCallees,
    const SEHJumpRuntime *JumpRuntime,
    const coff_loader::X86CxxHandlerABI *CxxHandler,
    const coff_loader::X86CxxPrologueCall *CxxPrologue,
    const std::vector<coff_loader::X86CxxEpilogueCall> *CxxEpilogues,
    const coff_loader::X86SEH4PrologueCall *SEH4Prologue,
    const std::vector<coff_loader::X86SEH4EpilogueCall> *SEH4Epilogues,
    const CxxCallerProof *CxxCalls)
    : Function(Function), EH(*Function.ExceptionMetadata),
      Chain(*EH.Registration), SecurityCookieVA(SecurityCookieVA),
      CookieCheckVA(CookieCheckVA), CxxHandler(CxxHandler),
      CxxPrologue(CxxPrologue), SEH4Prologue(SEH4Prologue),
      SEH4Epilogues(SEH4Epilogues), CxxEpilogues(CxxEpilogues),
      Callees(Callees), Cleanups(Cleanups), Stacks(Stacks),
      LocalUnwinds(LocalUnwinds), SEHControls(SEHControls),
      Conditional(Conditional), BoundCallees(BoundCallees),
      JumpRuntime(JumpRuntime), CxxCalls(CxxCalls),
      KnownCxx(EH.Encoding == ExceptionEncoding::X86CxxFuncInfo &&
               (EH.Personality == ExceptionPersonality::CxxFrameHandlerX86 ||
                EH.Personality == ExceptionPersonality::CxxFrameHandler3)),
      EH4(EH.Personality == ExceptionPersonality::ExceptHandler4),
      CheckCalls(Callees != nullptr),
      TrackRuntimeArguments(!KnownCxx && !Conditional && JumpRuntime &&
                            (JumpRuntime->Interlocked || JumpRuntime->Sleep)),
      CheckRuntimeObjects(KnownCxx && CheckCalls),
      CheckCleanups(KnownCxx && Cleanups != nullptr),
      InvalidBlocks(Function.Blocks.size()), States(Function.Blocks.size()),
      CompleteCalls(CheckCalls), CompleteCleanups(CheckCleanups),
      CompleteCatchObjects(CheckRuntimeObjects),
      CompleteRuntimeObjects(CheckRuntimeObjects) {}

RegistrationStateAnalysis RegistrationStateSolver::run(size_t InitialWork) {
  if (!charge(InitialWork))
    return std::move(Result);
  if (!initialize())
    return std::move(Result);
  if (!initializeOrder()) {
    Result.Diagnostics.push_back(
        "registration-state ordering budget exhausted");
    return std::move(Result);
  }
  if (!initializeContracts())
    return std::move(Result);
  if (SEHControls) {
    for (const auto &Control : *SEHControls)
      if (!charge(1) || Control.Address >= Control.EndAddress ||
          !SEHControlIndex.emplace(Control.Address, Control).second) {
        Result.SEHInvocationExitsComplete = false;
        Result.Diagnostics.push_back(
            "SEH physical control evidence is invalid");
        return std::move(Result);
      }
    Result.SEHControls = *SEHControls;
  }
  initializeCookies();
  collectOccurrences();
  seedEntries();
  while ((!Work.empty() || !PendingSEHFrames.empty()) && !Exhausted) {
    if (Work.empty()) {
      flushSEHScopes();
      continue;
    }
    const auto [Priority, I, Activation] = *Work.begin();
    Work.erase(Work.begin());
    auto &State = States[I].at(Activation);
    // Transfers can merge back into this node. Keep the input snapshot stable
    // while ordinary and exceptional successors update their own activations.
    const Domain Before = State.Incoming;
    if (!transferBlock(I, Before, State.Facts))
      return std::move(Result);
  }
  return finish();
}

bool RegistrationStateSolver::initialize() {
  const bool KnownSEH =
      (EH.Personality == ExceptionPersonality::ExceptHandler3 &&
       EH.Encoding == ExceptionEncoding::X86ScopeTableEH3) ||
      (EH.Personality == ExceptionPersonality::ExceptHandler4 &&
       EH.Encoding == ExceptionEncoding::X86ScopeTableEH4);
  Result.CxxContinuationsComplete = !KnownCxx;
  if ((!KnownSEH && !KnownCxx) || (KnownCxx != EH.Cxx.has_value())) {
    Result.Diagnostics.push_back(
        "registration language-handler semantics are not proven");
    return false;
  }
  if (EH.ParseStatus != ExceptionParseStatus::Complete) {
    Result.Diagnostics.push_back("registration metadata is incomplete");
    return false;
  }
  if (CxxPrologue && !validateCxxPrologue()) {
    Result.Diagnostics.push_back(
        "shared C++ prologue has no exact parent entry and call boundary");
    return false;
  }
  if (SEH4Prologue && !validateSEH4Prologue()) {
    Result.Diagnostics.push_back(
        "shared SEH4 prologue has no exact parent entry and call boundary");
    return false;
  }
  if (SEH4Epilogues)
    for (const auto &Call : *SEH4Epilogues)
      if (!charge(1) || !SEH4Prologue || !Call.Caller.isValid() ||
          Call.Caller.size() != 5 ||
          Call.Caller.Begin < SEH4Prologue->Caller.End ||
          Call.Caller.End > EH.CodeRange.End || !Call.Helper.isValid() ||
          Call.Helper.size() != 20 || EH.CodeRange.overlaps(Call.Helper) ||
          SEH4Prologue->Helper.overlaps(Call.Helper) ||
          !SEH4EpilogueIndex.emplace(Call.Caller.Begin, &Call).second) {
        Result.Diagnostics.push_back(
            "shared SEH4 epilogue has no unique physical call description");
        return false;
      }
  if (CxxEpilogues)
    for (const auto &Call : *CxxEpilogues)
      if (!charge(1) || !CxxPrologue || !Call.Caller.isValid() ||
          Call.Caller.End - Call.Caller.Begin != 5 ||
          Call.Caller.Begin < CxxPrologue->Caller.End ||
          Call.Caller.End > EH.CodeRange.End || !Call.Helper.isValid() ||
          Call.Helper.size() != (Call.CookieCheck ? 15u : 20u) ||
          EH.CodeRange.overlaps(Call.Helper) || !Call.Restore.isValid() ||
          Call.Restore.size() != 20 || EH.CodeRange.overlaps(Call.Restore) ||
          (Call.CookieCheck
               ? Call.Helper.overlaps(Call.Restore) ||
                     !Call.CookieCheck->Checker.isValid() ||
                     Call.CookieCheck->Checker.overlaps(EH.CodeRange) ||
                     Call.CookieCheck->Checker.overlaps(Call.Helper) ||
                     Call.CookieCheck->Checker.overlaps(Call.Restore)
               : Call.Helper.Begin != Call.Restore.Begin) ||
          !CxxEpilogueIndex.emplace(Call.Caller.Begin, &Call).second) {
        Result.Diagnostics.push_back(
            "shared C++ epilogue has no unique physical call description");
        return false;
      }
  if (Chain.RealignedFrame && !validateRealignedLayout()) {
    Result.Diagnostics.push_back("realigned registration frame requires a "
                                 "separate coordinate transfer proof");
    return false;
  }
  if (!Chain.RegistrationOffset || !Chain.TryLevelOffset ||
      !Chain.SeededTryLevel || !Chain.ChainInstallVA ||
      (KnownCxx && (!Chain.cxxRuntimeFrameOffset() ||
                    *Chain.RegistrationOffset < INT32_MIN + 4))) {
    Result.Diagnostics.push_back(
        "registration frame and state slot are not proven");
    return false;
  }

  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    if (!Index.emplace(Block.Id, I).second ||
        !Entries.emplace(Block.StartAddr, I).second ||
        Block.StartAddr >= Block.EndAddr) {
      Result.Diagnostics.push_back("invalid registration-state CFG identity");
      return false;
    }
  }
  auto Entry = Entries.find(Function.Entry);
  if (Entry == Entries.end()) {
    Result.Diagnostics.push_back("registration-state CFG has no exact entry");
    return false;
  }

  const size_t StateCount = EH.Cxx ? EH.Cxx->MaxState : Chain.Scopes.size();
  if (StateCount > limits::kMaxRegistrationEHRecords) {
    Result.Diagnostics.push_back(
        "registration-state count exceeds the work budget");
    return false;
  }
  if (EH.Cxx) {
    size_t Remaining = limits::kMaxRegistrationEHRecords - StateCount;
    if (EH.Cxx->TryBlocks.size() > Remaining) {
      Result.Diagnostics.push_back("C++ registration graph exceeds the budget");
      return false;
    }
    Remaining -= EH.Cxx->TryBlocks.size();
    for (const CxxTryBlock &Try : EH.Cxx->TryBlocks) {
      if (Try.Handlers.size() > Remaining) {
        Result.Diagnostics.push_back(
            "C++ registration graph exceeds the budget");
        return false;
      }
      Remaining -= Try.Handlers.size();
    }
    if (!EH.Cxx->hasValidStateGraph()) {
      Result.Diagnostics.push_back("C++ registration state graph is invalid");
      return false;
    }
  }
  if (!KnownCxx) {
    if (!charge(StateCount + 1) ||
        !hasValidRegistrationScopeGraph(Chain, EH4)) {
      Result.Diagnostics.push_back("invalid SEH registration scope graph");
      return false;
    }
    // The frame sentinel remains a valid state even if installation starts
    // directly in a checked active scope.
    AllLevels.insert(registrationOutsideTryLevel(EH4));
    for (const auto &Scope : Chain.Scopes)
      if (Scope.IsFinally)
        FinallyTargets.insert(Scope.HandlerVA);
  }
  AllLevels.insert(*Chain.SeededTryLevel);
  for (size_t I = 0; I < StateCount; ++I)
    AllLevels.insert(static_cast<int32_t>(I));
  // Byte observations are not transfers until both their instruction and the
  // lifted address/value have been authenticated.
  for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores) {
    auto Owner = Entries.upper_bound(Store.StoreVA);
    if (Owner == Entries.begin())
      continue;
    const size_t I = std::prev(Owner)->second;
    const LowBlock &Block = Function.Blocks[I];
    if (Store.StoreVA >= Block.EndAddr)
      continue;
    auto Boundary = std::find_if(
        Block.InstructionBoundaries.begin(), Block.InstructionBoundaries.end(),
        [&](const LowInstructionBoundary &Instruction) {
          return Instruction.Address == Store.StoreVA &&
                 Store.EndVA > Store.StoreVA &&
                 Instruction.Size == Store.EndVA - Store.StoreVA;
        });
    const bool ValidImmediate =
        Store.Width == 4   ? validLevel(Store.Level)
        : Store.Width == 1 ? uint32_t(Store.Level) <= UINT8_MAX
        : Store.Width == 2 ? uint32_t(Store.Level) <= UINT16_MAX
                           : false;
    if (!ValidImmediate || Store.EndVA != Block.EndAddr ||
        Boundary == Block.InstructionBoundaries.end() ||
        !Stores.emplace(Store.StoreVA, Store).second)
      InvalidBlocks[I] = true;
  }
  bool HasInstallBoundary = false;
  for (const LowBlock &Block : Function.Blocks)
    for (const LowInstructionBoundary &Instruction :
         Block.InstructionBoundaries)
      HasInstallBoundary |=
          Instruction.Address == Chain.ChainInstallVA &&
          Instruction.Size != 0 &&
          Instruction.Control == (CxxPrologue || SEH4Prologue
                                      ? LowInstructionControl::Call
                                      : LowInstructionControl::None) &&
          Instruction.Address <= InvalidVA - Instruction.Size &&
          Instruction.Address + Instruction.Size == Block.EndAddr;
  if (!HasInstallBoundary) {
    Result.Diagnostics.push_back(
        "registration installation is not an exact decoded boundary");
    return false;
  }
  return true;
}

bool RegistrationStateSolver::charge(size_t Amount) {
  if (Amount > limits::kMaxRegistrationEHStateWork - WorkUsed) {
    WorkUsed = limits::kMaxRegistrationEHStateWork;
    Exhausted = true;
    Result.Complete = Result.CallbackStatesComplete =
        Result.RegistrationLifetimeComplete = false;
    Result.SEHInvocationExitsComplete = false;
    Result.SEHInvocationExits.clear();
    Result.SEHJumpResumptionsComplete = false;
    Result.SEHJumpResumptions.clear();
    Result.SEHJumpCapturesComplete = false;
    Result.SEHJumpCaptures.clear();
    Result.SEHStateStoresComplete = false;
    Result.SEHStateStores.clear();
    Result.SEHBoundCallsComplete = false;
    Result.SEHBoundCalls.clear();
    Result.SEHExceptionAccessesComplete = false;
    Result.SEHExceptionAccesses.clear();
    return false;
  }
  WorkUsed += Amount;
  return true;
}

} // namespace neverd::registration_state

namespace neverd {
RegistrationStateAnalysis analyzeRegistrationStates(
    const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls,
    const coff_loader::X86CxxHandlerABI *CxxHandler,
    const coff_loader::X86CxxPrologueCall *CxxPrologue) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return {};
  return registration_state::RegistrationStateSolver(
             Function, SecurityCookieVA, CookieCheckVA, Callees, Cleanups,
             Stacks, LocalUnwinds, SEHControls, nullptr, nullptr, nullptr,
             CxxHandler, CxxPrologue)
      .run();
}
} // namespace neverd
