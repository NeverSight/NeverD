//===- RegistrationState.cpp - x86 EH state analysis ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

RegistrationStateSolver::RegistrationStateSolver(
    const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups)
    : Function(Function), EH(*Function.ExceptionMetadata),
      Chain(*EH.Registration), SecurityCookieVA(SecurityCookieVA),
      CookieCheckVA(CookieCheckVA), Callees(Callees), Cleanups(Cleanups),
      KnownCxx(EH.Encoding == ExceptionEncoding::X86CxxFuncInfo &&
               (EH.Personality == ExceptionPersonality::CxxFrameHandlerX86 ||
                EH.Personality == ExceptionPersonality::CxxFrameHandler3)),
      EH4(EH.Personality == ExceptionPersonality::ExceptHandler4),
      CheckCalls(Callees != nullptr),
      CheckRuntimeObjects(KnownCxx && CheckCalls),
      CheckCleanups(KnownCxx && Cleanups != nullptr),
      Facts(Function.Blocks.size()), Incoming(Function.Blocks.size()),
      Queued(Function.Blocks.size()), CompleteCalls(CheckCalls),
      CompleteCleanups(CheckCleanups),
      CompleteCatchObjects(CheckRuntimeObjects),
      CompleteRuntimeObjects(CheckRuntimeObjects) {}

RegistrationStateAnalysis RegistrationStateSolver::run() {
  if (!initialize() || !initializeContracts())
    return std::move(Result);
  initializeCookies();
  collectOccurrences();
  seedEntries();
  while (!Work.empty() && !Exhausted) {
    const size_t I = Work.front();
    Work.pop_front();
    Queued[I] = false;
    if (!transferBlock(I))
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
  if (Chain.RealignedFrame && !validateRealignedLayout()) {
    Result.Diagnostics.push_back("realigned registration frame requires a "
                                 "separate coordinate transfer proof");
    return false;
  }
  if (!Chain.RegistrationOffset || !Chain.TryLevelOffset ||
      !Chain.SeededTryLevel || !Chain.ChainInstallVA ||
      Chain.TryLevelStores.empty() ||
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
      Facts[I].Invalid = true;
  }
  bool HasInstallBoundary = false;
  for (const LowBlock &Block : Function.Blocks)
    for (const LowInstructionBoundary &Instruction :
         Block.InstructionBoundaries)
      HasInstallBoundary |=
          Instruction.Address == Chain.ChainInstallVA &&
          Instruction.Size == Chain.chainInstallInstructionSize() &&
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
    Exhausted = true;
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
    const std::vector<RegistrationCleanupFrameContract> *Cleanups) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return {};
  return registration_state::RegistrationStateSolver(
             Function, SecurityCookieVA, CookieCheckVA, Callees, Cleanups)
      .run();
}
} // namespace neverd
