//===- RegistrationStateNestedFinally.cpp - Nested near CALL returns ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks nested finally resumptions against the active capture and value.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::returnNestedFinally(
    Domain &After, const LowOp &Op, BlockFacts &Facts,
    RegistrationSEHInvocationExit Exit) {
  const SEHExitKey Identity{Exit.EntryVA, Exit.CallVA, Exit.Address,
                            Exit.OpSeq};
  const auto Refuse = [&] {
    SEHExits.erase(Identity);
    InvalidSEHExits.insert(Identity);
    Result.SEHInvocationExitsComplete = false;
    Facts.Invalid = After.Unknown = true;
    After.Levels.clear();
    return true;
  };
  const auto Return = SEHControlIndex.find(Op.Addr);
  const auto Call = SEHControlIndex.find(After.Activation.OrdinaryCall);
  const auto Target = sehCallContinuation(
      After.Activation.OrdinaryCall,
      After.SEHCallers.empty()
          ? 0
          : After.SEHCallers.back().Activation.Entry.value_or(0));
  if (!callbackCanReturn(After) || Return == SEHControlIndex.end() ||
      Return->second.Control !=
          RegistrationSEHControlContract::Kind::NearReturn ||
      Call == SEHControlIndex.end() ||
      Call->second.Control != RegistrationSEHControlContract::Kind::NearCall ||
      Call->second.Target != After.Activation.Entry || !Target ||
      After.SEHCallers.empty() ||
      !charge(After.sehCallerCellCount() + After.Frame.cellCount() + 10))
    return Refuse();
  const auto &Caller = After.SEHCallers.back();
  const auto SP = Caller.Stack.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                      .CallbackAddress;
  if (!Caller.Activation.Entry || Caller.Activation.SearchFilter || !SP ||
      SP->Entry != Caller.Activation.Entry || SP->Offset > 0 ||
      Caller.Stack.CallbackEntry != Caller.Activation.Entry)
    return Refuse();
  Exit.TargetVA = *Target;
  Exit.StackOffset = SP->Offset;
  if (InvalidSEHExits.count(Identity))
    return Refuse();
  const auto [Found, Added] = SEHExits.emplace(Identity, Exit);
  if (!Added && Found->second != Exit)
    return Refuse();
  Domain Resume = After;
  Resume.Frame.leaveCallback();
  Resume.Frame.CallbackEntry = Caller.Stack.CallbackEntry;
  Resume.Frame.CallbackCells = Caller.Stack.CallbackCells;
  Resume.Frame.InitializedCallbackBytes = Caller.Stack.InitializedCallbackBytes;
  Resume.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::callbackFrame(SP->Entry, SP->Offset);
  Resume.Activation = Caller.Activation;
  Resume.OtherCallback = Caller.OtherCallback;
  Resume.OrdinaryCallStack = Caller.OrdinaryCallStack;
  Resume.StackRestore = Caller.StackRestore;
  Resume.SEHCallers.pop_back();
  merge(Entries.at(*Target), Resume);
  Facts.InvocationAtExit = true;
  return true;
}

} // namespace neverd::registration_state
