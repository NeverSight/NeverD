//===- RegistrationStateConditional.cpp - Checked callee borrows ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific x86 registration frame inputs and bounded effects.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include <utility>

namespace neverd::registration_state {

std::optional<RegistrationStateSolver::CallTransfer>
RegistrationStateSolver::transferConditionalCxxCall(Domain &After,
                                                    const LowOp &Op,
                                                    const BlockFacts &Facts) {
  if (!KnownCxx || !Conditional || !Conditional->ProveCxxCall ||
      After.Unknown || Facts.Invalid || !After.Parent || After.Callback ||
      !After.Installed || After.Uninstalled || Chain.RealignedFrame ||
      Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Seq < 0)
    return std::nullopt;
  const auto SP = parentStackOffset(After);
  if (!SP)
    return std::nullopt;
  const auto Effect = Conditional->ProveCxxCall(
      Function, Op, After.Frame, After.InitializedFrameBytes, WorkUsed);
  if (!Effect || !Effect->StackPopBytes || Effect->StackPopBytes > UINT16_MAX ||
      Effect->StackPopBytes % 4 || Effect->Returns.empty() ||
      int64_t(*SP) + Effect->StackPopBytes > *Chain.cxxRuntimeFrameOffset() ||
      !frameObjectIsDisjointFromAdministration(
          *SP, int64_t(*SP) + Effect->StackPopBytes) ||
      !charge(Effect->StackPopBytes))
    return std::nullopt;
  const int32_t End = int32_t(int64_t(*SP) + Effect->StackPopBytes);
  for (int64_t Byte = *SP; Byte < End; ++Byte)
    if (!After.InitializedFrameBytes.count(int32_t(Byte)))
      return std::nullopt;

  // The caller owns the storage and its runtime administration. Check even
  // unused borrows, without inventing a read or a write for their admission.
  std::vector<RegistrationObjectExtent> Objects, Reads, Writes;
  if (!projectFrameObject(After, 0, SP, Effect->Objects, Objects, true,
                          false) ||
      !projectFrameObject(After, 0, SP, Effect->Reads, Reads, true) ||
      !projectFrameObject(After, 0, SP, Effect->Writes, Writes, false) ||
      !charge(Effect->ImageReads.size() + Effect->Returns.size() + 1))
    return std::nullopt;
  for (const auto &Read : Effect->ImageReads)
    ImageReads.emplace(Read.Begin, Read.End);
  for (const auto &Write : Writes) {
    if (!charge(After.Frame.cellCount() + 1))
      return std::nullopt;
    After.Frame.forgetCellValues(Write.Begin,
                                 uint32_t(Write.End - Write.Begin));
  }
  std::optional<FrameValue> Returned;
  for (const auto &Return : Effect->Returns) {
    const auto Value = Return.BorrowedOffset
                           ? FrameValue::frame(*Return.BorrowedOffset)
                           : FrameValue::taint(Return.MayAliasFrame);
    Returned = Returned ? join(*Returned, Value) : Value;
  }
  if (!charge(After.InitializedFrameBytes.size() + After.Frame.cellCount() +
              After.RuntimeObject.cellCount()))
    return std::nullopt;
  After.InitializedFrameBytes.erase(
      After.InitializedFrameBytes.begin(),
      After.InitializedFrameBytes.lower_bound(End));
  for (auto *Frame : {&After.Frame, &After.RuntimeObject})
    discardFrameCellsBelow(Frame->Cells, End);
  // The callback authenticated the unique original five-byte CALL. No
  // context-free CallFrameEffect or native ABI is published for this result.
  return CallTransfer{FrameValue::frame(End), false, Op.Addr + 5, Returned,
                      std::move(Writes)};
}

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
  // Defer a direct candidate to the source invocation owner. Neither this
  // gate nor an independent stack-pop declaration proves a C++ callee effect.
  if (KnownCxx && CheckCalls && Conditional->ProveCxxCall &&
      Op.Opcode == NdOp::CALL)
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
