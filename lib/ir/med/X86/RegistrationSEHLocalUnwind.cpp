//===- RegistrationSEHLocalUnwind.cpp - Source local unwind binding ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the actual invocation, state relation and absence of a scalar result.

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationCallResult.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/ir/med/X86/X86RegistrationSEHLocalUnwind.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {

std::optional<std::map<va_t, X86RegistrationSEHLocalUnwind>>
getX86RegistrationSEHLocalUnwinds(const MedFunc &Function) {
  if (!Function.RegistrationStates)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  std::map<va_t, X86RegistrationSEHLocalUnwind> Result;
  if (!States.HasSEHLocalUnwinds && States.SEHLocalUnwinds.empty())
    return Result;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.SEHLocalUnwindsComplete ||
      !States.HasSEHLocalUnwinds || States.SEHLocalUnwinds.empty() ||
      !Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Chain = *EH.Registration;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  const int32_t Outside = EH4 ? -2 : -1;
  if (EH.ParseStatus != ExceptionParseStatus::Complete || EH.Cxx ||
      (!EH4 && EH.Personality != ExceptionPersonality::ExceptHandler3) ||
      EH.Encoding != (EH4 ? ExceptionEncoding::X86ScopeTableEH4
                          : ExceptionEncoding::X86ScopeTableEH3) ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != Outside ||
      (EH4 && (!States.SecurityCookiesComplete || !States.SecurityCookieVA ||
               States.SecurityCookieVA > UINT32_MAX - 3)))
    return std::nullopt;
  const auto Owners = registrationSEHCallbackOwners(Function);
  if (!Owners)
    return std::nullopt;
  size_t Work = 0;
  const auto ValidLevel = [&](int32_t Level) {
    return Level == Outside ||
           (Level >= 0 && size_t(Level) < Chain.Scopes.size());
  };
  for (const auto &Call : States.SEHLocalUnwinds) {
    const auto &Runtime = Call.Runtime;
    const unsigned Size = Runtime.Indirect ? 6 : 5;
    if (++Work > limits::kMaxRegistrationEHStateWork || Call.OpSeq < 0 ||
        Call.Address > UINT32_MAX - Size ||
        Call.EndAddress != Call.Address + Size || Runtime.Target > UINT32_MAX ||
        !Runtime.Target || !Runtime.ImportSlot ||
        Runtime.ImportSlot > UINT32_MAX - 3 ||
        (Runtime.Indirect && Runtime.Target != Runtime.ImportSlot) ||
        Runtime.Protocol !=
            (EH4 ? RegistrationLocalUnwindContract::Kind::EH4
                 : RegistrationLocalUnwindContract::Kind::EH3) ||
        Call.StackOffset > -16 - (EH4 ? 12 : 8) ||
        Call.StackOffset < -int64_t(limits::kMaxSourceCallBorrowedBytes) ||
        Call.EntryVA > UINT32_MAX || Call.Transitions.empty() ||
        !Result.emplace(Call.Address, X86RegistrationSEHLocalUnwind{&Call})
             .second)
      return std::nullopt;
    std::optional<std::pair<int32_t, int32_t>> Previous;
    for (const auto &Transition : Call.Transitions) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          !ValidLevel(Transition.first) || !ValidLevel(Transition.second) ||
          (Previous && *Previous >= Transition) ||
          (Transition.second != Outside &&
           (Call.Bound == uint32_t(Outside) ||
            uint32_t(Transition.second) > Call.Bound)))
        return std::nullopt;
      Previous = Transition;
    }
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Found = Result.find(Op.Addr);
      if (Found == Result.end() ||
          (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL))
        continue;
      auto &Call = Found->second;
      const auto &Source = *Call.Source;
      const auto Owner = Owners->find(Block.Id);
      if (Call.Operation || Op.Dead || Op.ExactArguments || Op.SourceCallHint ||
          Op.OriginSeq != Source.OpSeq ||
          Op.Opcode !=
              (Source.Runtime.Indirect ? NdOp::INDIR_CALL : NdOp::CALL) ||
          Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Size != 4 ||
          Op.Inputs[0].ConstVal != Source.Runtime.Target ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          (Source.EntryVA ? Owner == Owners->end() || !Owner->second.second ||
                                Owner->second.first != Source.EntryVA
                          : Owner != Owners->end()))
        return std::nullopt;
      Call.Operation = &Op;
      Call.Block = Block.Id;
    }
  for (const auto &[Address, Call] : Result) {
    if (!Call.Operation)
      return std::nullopt;
    RegistrationCallFrameEffect Effect;
    Effect.Address = Address;
    Effect.OpSeq = Call.Source->OpSeq;
    Effect.Target = Call.Source->Runtime.Target;
    if (!hasUnusedX86RegistrationCallResult(Function, Effect, Work,
                                            Call.Source->Runtime.Indirect))
      return std::nullopt;
  }
  return Result;
}

} // namespace neverd
