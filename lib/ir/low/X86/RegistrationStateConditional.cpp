//===- RegistrationStateConditional.cpp - Checked callee borrows ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::checkConditionalMemory(
    const FrameState &State, const LowOp &Op, const FrameTransfer &Transfer,
    const std::set<int32_t> *Initialized) {
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Address)
    return true;
  auto Refuse = [&] {
    Conditional->Complete = false;
    return false;
  };
  if (!Memory.Complete || Op.MemoryOrdering != NdMemoryOrdering::None ||
      (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE))
    return Refuse();
  const auto Address = Transfer.read(*Memory.Address);
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
    return (Address.Constant == 0 && Memory.AccessSize == 4) || Refuse();
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return Refuse();
  if ((EH4 || CxxCookies) && CompleteCookies && Op.Opcode == NdOp::LOAD &&
      Memory.AccessSize == 4 && Address.Constant == SecurityCookieVA)
    return true;
  if (Address.SEHException)
    return (Op.Opcode == NdOp::LOAD &&
            State.sehExceptionLoad(*Address.SEHException, Memory.AccessSize)
                .has_value()) ||
           Refuse();
  if (Address.CallbackAddress)
    return State.callbackMemoryIsPrivate(*Address.CallbackAddress,
                                         Memory.AccessSize,
                                         Op.Opcode == NdOp::LOAD) ||
           Refuse();
  if (!Address.Offset)
    return Refuse();
  const int64_t End = int64_t(*Address.Offset) + Memory.AccessSize;
  if (*Address.Offset < 4) {
    const auto SP =
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
    const auto SavedSP = State.load(*Chain.RegistrationOffset - 8, 4).Offset;
    // The runtime enters an except body before its first SavedESP reload.
    // Admit only that initialized administration read at the exact handler
    // entry; every later private access still needs the restored SP.
    const bool HandlerRestore =
        !SP && Op.Opcode == NdOp::LOAD && Memory.AccessSize == 4 &&
        *Address.Offset == *Chain.RegistrationOffset - 8 &&
        SEHParentEntries.count(Op.Addr);
    const auto Allocation = SP ? SP
                            : (State.CallbackEntry || HandlerRestore)
                                ? SavedSP
                                : std::nullopt;
    if (!Allocation || *Address.Offset < *Allocation || End > 4)
      return Refuse();
    // Only the current runtime search invocation initializes this slot.
    const bool ExceptionSlot = Op.Opcode == NdOp::LOAD &&
                               State.SEHFilterEntry && *Address.Offset == -20 &&
                               Memory.AccessSize == 4 &&
                               State.load(-20, 4).SEHException;
    if (Op.Opcode == NdOp::LOAD && !ExceptionSlot) {
      if (!Initialized || !charge(Memory.AccessSize))
        return Refuse();
      for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
        if (!Initialized->count(int32_t(Byte)))
          return Refuse();
    }
    return true;
  }
  if (*Address.Offset == 4)
    return (Op.Opcode == NdOp::LOAD && Memory.AccessSize == 4) || Refuse();
  if (Conditional->Arguments.count(*Address.Offset)) {
    Conditional->UsedArguments.insert(*Address.Offset);
    return (Op.Opcode == NdOp::LOAD && Memory.AccessSize == 4) || Refuse();
  }
  if (End > INT32_MAX ||
      !llvm::any_of(Conditional->Objects, [&](const auto &Object) {
        return Object.Begin <= *Address.Offset && End <= Object.End;
      }))
    return Refuse();
  if (Op.Opcode == NdOp::STORE && Transfer.read(*Memory.StoredValue).MayBeFrame)
    return Refuse();
  auto &Effects =
      Op.Opcode == NdOp::LOAD ? Conditional->Reads : Conditional->Writes;
  Effects.emplace(*Address.Offset, int32_t(End));
  return true;
}

bool RegistrationStateSolver::conditionalReturn(const Domain &State,
                                                const LowOp &Op) {
  if (Op.Opcode != NdOp::RETURN || State.Callback)
    return true;
  bool Valid =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset ==
          4 &&
      State.Frame.load(4, 4).ReturnPC;
  for (const auto Reg : {x86reg::RBX, x86reg::RBP, x86reg::RSI, x86reg::RDI}) {
    const unsigned Index = Reg / x86reg::GeneralRegStride;
    const auto &Value = State.Frame.Registers[Index];
    Valid &= Value.SavedRegister == Index + 1 && !Value.Offset &&
             !Value.CallbackAddress && !Value.SEHException;
  }
  Conditional->Complete &= Valid;
  if (Valid && KnownCxx && charge(1)) {
    const auto Value =
        State.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride];
    const auto [It, New] = Conditional->ReturnValues.emplace(
        std::make_pair(Op.Addr, Op.Seq), Value);
    if (!New)
      It->second = join(It->second, Value);
  }
  return Valid;
}

bool RegistrationStateSolver::conditionalCall(const FrameState &State,
                                              const LowOp &Op) {
  if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
    return true;
  auto Refuse = [&] {
    Conditional->Complete = false;
    return false;
  };
  if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
    return Refuse();
  if (KnownCxx) {
    const auto SP =
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
    // CALL writes its return PC below ESP even without an explicit LowIR
    // STORE. It cannot borrow the incoming/output area or overwrite a node.
    if (!SP || *SP > *Chain.RegistrationOffset)
      return Refuse();
  }
  const va_t Target = Op.Inputs[0].Offset;
  // The transfer owner checks the actual ECX cookie and records the call.
  // The callee proof requires that complete cookie receipt before publishing.
  if ((EH4 || CxxCookies) && CompleteCookies && CookieCheckVA &&
      Op.Opcode == NdOp::CALL && Target == CookieCheckVA)
    return true;
  // These contracts have independently checked original bodies, including
  // ambient memory accesses. transferCall still has to bind the current
  // arguments, initialization and effects; stack-pop evidence alone is not
  // a no-capture contract.
  if (KnownCxx && CheckCalls && Op.Opcode == NdOp::CALL &&
      CalleeIndices.count(Target))
    return true;
  if (LocalUnwindTargets.count({Target, Op.Opcode == NdOp::INDIR_CALL}) ||
      (Op.Opcode == NdOp::CALL && FinallyTargets.count(Target)))
    return true;
  // Authentication grants only entry to the argument/activation checker.
  // An absent or expired borrowed capture still refuses at the transfer.
  if (JumpRuntime && JumpRuntime->Resume && JumpRuntime->Resume(Op, WorkUsed))
    return true;
  if (!Conditional->ProveCall || !Conditional->ProveCall(Op, State))
    return Refuse();
  return true;
}

} // namespace neverd::registration_state
