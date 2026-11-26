//===- ReturningCallState.cpp - Bounded conditional call effect ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The sole machine-state implementation used by conditional call recovery.
//===----------------------------------------------------------------------===//

#include "ReturningCallState.h"

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymState.h"

namespace neverd::analysis {
using namespace symbolic;

void applyReturningCallState(SymState &State, SymRef FrameRoot,
                             SymRef CallerStack, int64_t PreservedBegin,
                             int64_t FrameEnd, va_t GuestContinuation,
                             const RecoveryCallTransition &Policy,
                             ReturningCallStateEffect &Effect,
                             llvm::ArrayRef<ReturningCallFrameOutput> Outputs) {
  Effect.WorkUsed = 0;
  Effect.Status = SpecializationStatus::InvalidInput;
  Effect.Diagnostic = "returning-call state effect was not applied";
  const auto fail = [&](SpecializationStatus Status, const char *Diagnostic) {
    Effect.Status = Status;
    Effect.Diagnostic = Diagnostic;
  };
  const auto charge = [&](uint64_t Count) {
    if (Count > Effect.MaxWork - Effect.WorkUsed) {
      fail(SpecializationStatus::BudgetExceeded,
           "returning-call transition/provenance budget exhausted");
      return false;
    }
    Effect.WorkUsed += Count;
    return true;
  };
  auto &Ctx = State.context();
  // The architecture owner supplies this finite policy. Keep the same output
  // reservation before either forgetting primitive as the integrated caller.
  const uint64_t WriteNodes = Policy.FreshFlags.size() * 2 + 1;
  const uint64_t WriteBytes =
      Policy.Stack.Size + Policy.ProgramCounter.Size + Policy.FreshFlags.size();
  if (!charge(WriteBytes) || !charge(WriteBytes))
    return;
  if (Ctx.numNodes() > Effect.MaxNodes ||
      WriteNodes > Effect.MaxNodes - Ctx.numNodes()) {
    fail(SpecializationStatus::BudgetExceeded,
         "returning-call output node allowance exhausted");
    return;
  }
  SymRegisterPreservation Registers;
  Registers.Ranges = Policy.PreservedRegisters;
  Registers.MaxWork = Effect.MaxWork - Effect.WorkUsed;
  Registers.MaxNewNodes = Effect.MaxNodes - Ctx.numNodes() - WriteNodes;
  State.clobberRegistersExcept(Registers);
  Effect.WorkUsed += Registers.WorkUsed;
  if (Registers.Status != SymRegisterPreservationStatus::Applied) {
    fail(Registers.Status == SymRegisterPreservationStatus::BudgetExceeded
             ? SpecializationStatus::BudgetExceeded
             : SpecializationStatus::InvalidInput,
         "returning-call register preservation refused");
    return;
  }
  SymMemoryPreservation Memory;
  Memory.Base = FrameRoot;
  Memory.Begin = PreservedBegin;
  Memory.End = FrameEnd;
  Memory.EarlierRanges = Policy.PreservedFrameRanges;
  Memory.MaxWork = Effect.MaxWork - Effect.WorkUsed;
  State.clobberMemory(Memory);
  Effect.WorkUsed += Memory.WorkUsed;
  if (Memory.Status != SymMemoryPreservationStatus::Applied) {
    fail(Memory.Status == SymMemoryPreservationStatus::BudgetExceeded
             ? SpecializationStatus::BudgetExceeded
             : SpecializationStatus::InvalidInput,
         "returning-call memory preservation refused");
    return;
  }
  for (uint64_t Flag : Policy.FreshFlags)
    State.write(SymSpace::Register, Flag,
                Ctx.mkZExt(Ctx.mkFreshVar(1, "returning_call_flag"), 8));
  State.write(SymSpace::Register, Policy.Stack.Offset, CallerStack);
  State.write(SymSpace::Register, Policy.ProgramCounter.Offset,
              Ctx.mkConst(Policy.ProgramCounter.Size * 8, GuestContinuation));
  for (const auto &Output : Outputs)
    for (uint16_t I = 0; I < Output.Bytes; ++I) {
      if (!charge(2))
        return;
      const uint64_t Offset = Output.Offset + I;
      const SymRef Address = Ctx.mkAdd(FrameRoot, Ctx.mkConst(64, Offset));
      // A valid symbolic-region store returns false when it must forget other
      // regions. Check the written byte itself, not that alias-preservation
      // result, before accepting the effect.
      State.store(Address, Ctx.mkFreshVar(8, "returning_call_output"));
      if (!State.hasMaterializedRegionRange(FrameRoot, Offset, 1)) {
        fail(SpecializationStatus::InvalidInput,
             "returning-call frame output write was refused");
        return;
      }
      if (Ctx.numNodes() > Effect.MaxNodes) {
        fail(SpecializationStatus::BudgetExceeded,
             "returning-call frame output node budget exhausted");
        return;
      }
    }
  Effect.Status = SpecializationStatus::Complete;
  Effect.Diagnostic = "";
}

} // namespace neverd::analysis
