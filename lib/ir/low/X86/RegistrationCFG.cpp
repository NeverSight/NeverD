//===- RegistrationCFG.cpp - PE32 exception control-flow closure --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reconcile authenticated throw termination and runtime continuation roots.
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include <algorithm>
#include <set>
#include <string>

namespace neverd {

void CFGBuilder::closeRegistrationCxxContinuations(const BinaryImage &Img,
                                                   Decoder &Dec,
                                                   LowFunc &Func) {
  if (Img.Arch != Arch::X86 || !Func.ExceptionMetadata ||
      !Func.ExceptionMetadata->Registration || !Func.ExceptionMetadata->Cxx ||
      !Func.RegistrationStates)
    return;

  std::set<va_t> AddedRoots;
  std::set<va_t> AddedPersistent;
  std::set<va_t> Quarantined;
  std::set<std::string> FailureDiagnostics;
  size_t WorkUsed = 0;
  bool Failed = false;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - WorkUsed) {
      Failed = true;
      return false;
    }
    WorkUsed += Amount;
    return true;
  };
  for (;;) {
    const auto &Diagnostics = Func.RegistrationStates->Diagnostics;
    FailureDiagnostics.insert(Diagnostics.begin(), Diagnostics.end());
    const auto &Callees = Func.RegistrationStates->CalleeContracts;
    if (!Charge(Callees.size() + Insns.size() + 1))
      break;
    std::set<va_t> NoReturnTargets;
    for (const auto &Callee : Callees)
      if (Callee.DoesNotReturn)
        NoReturnTargets.insert(Callee.Target);
    // The source-state proof already authenticates these immutable callees.
    // Its noreturn fact must also close ordinary CFG reachability. Otherwise
    // dead bytes after a private throw can branch into a callback instruction
    // and corrupt its boundaries. Independent runtime/address-taken roots
    // remain live when rebuildBlocks recomputes reachability.
    bool ChangedCalls = false;
    for (auto &[Address, Instruction] : Insns) {
      if (!Instruction.IsCall || Instruction.IsNoReturnCall)
        continue;
      if (!Charge(Instruction.Ops.size()))
        break;
      for (const auto &Op : Instruction.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
            NoReturnTargets.count(Op.Inputs[0].Offset)) {
          Instruction.IsNoReturnCall = ChangedCalls = true;
          break;
        }
    }
    const auto Candidates = Func.RegistrationStates->CxxContinuations;
    if (Failed || !Charge(Candidates.size() + Func.Blocks.size() + 1))
      break;
    std::set<va_t> ProvenTargets;
    bool Grew = false;
    for (const RegistrationCxxContinuation &Continuation : Candidates) {
      const va_t Target = Continuation.TargetVA;
      if (Quarantined.count(Target) || !isOwnedInteriorTarget(Img, Target)) {
        Failed = true;
        continue;
      }
      ProvenTargets.insert(Target);
      if (!DurableCFGRoots.count(Target) || !BlockStarts.count(Target) ||
          !Insns.count(Target)) {
        if (!AddedRoots.count(Target) &&
            AddedRoots.size() == limits::kMaxRegistrationEHRecords) {
          Failed = true;
          break;
        }
        BlockStarts.insert(Target);
        if (DurableCFGRoots.insert(Target).second)
          AddedRoots.insert(Target);
        if (PersistentCFGRoots.insert(Target).second)
          AddedPersistent.insert(Target);
        // Runtime resumption supplies a reaching frame/state; this is not an
        // independent ordinary entry with unknown incoming registers.
        if (!ExploredAddrs.count(Target))
          explore(Img, Dec, Target);
        Grew = true;
      }
      if (!Insns.count(Target)) {
        Failed = true;
        Quarantined.insert(Target);
        ProvenTargets.erase(Target);
      }
    }
    bool Withdrew = false;
    for (auto It = AddedRoots.begin(); It != AddedRoots.end();) {
      if (ProvenTargets.count(*It)) {
        ++It;
        continue;
      }
      if (AddedPersistent.erase(*It))
        PersistentCFGRoots.erase(*It);
      DurableCFGRoots.erase(*It);
      Quarantined.insert(*It);
      It = AddedRoots.erase(It);
      Withdrew = Failed = true;
    }
    if (!ChangedCalls && !Grew && !Withdrew)
      break;
    splitBlocks();
    rebuildBlocks(Func);
    multiStageResolve(Img, Dec, Func);
    convertIndirectTailCalls(Func);
  }
  if (Failed) {
    auto &State = *Func.RegistrationStates;
    // Rebuilding after withdrawing a candidate may succeed on the smaller
    // graph. Retain the failure that caused that withdrawal, including state
    // work exhaustion, rather than reporting only a missing continuation.
    for (const auto &Diagnostic : FailureDiagnostics)
      if (llvm::find(State.Diagnostics, Diagnostic) == State.Diagnostics.end())
        State.Diagnostics.push_back(Diagnostic);
    State.Complete = State.CallbackStatesComplete =
        State.CxxContinuationsComplete = State.RegistrationLifetimeComplete =
            State.ChainOperationsComplete = State.ImageReadsComplete = false;
    State.ChainAccesses.clear();
    State.Diagnostics.push_back(
        "C++ continuation ownership or CFG closure is not proven");
  }
}

} // namespace neverd
