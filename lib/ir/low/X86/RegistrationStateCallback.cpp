//===- RegistrationStateCallback.cpp - x86 callback return contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

std::optional<int32_t>
RegistrationStateSolver::parentStackOffset(const Domain &State) const {
  const auto &SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (SP.Offset)
    return SP.Offset;
  if (!Chain.hasCxxCallbackStack() || !SP.CallbackAddress ||
      SP.CallbackAddress->Entry != State.Frame.CallbackEntry ||
      SP.CallbackAddress->Offset > 0 ||
      SP.CallbackAddress->Offset <
          -int64_t(limits::kMaxRegistrationEHStateWork) ||
      State.Parent || !State.Callback || State.OtherCallback ||
      State.CxxCatchStacks.size() != 1 ||
      State.CxxCatchStacks.begin()->size() != 1)
    return std::nullopt;
  // Callback allocation cannot release parent locals. SavedESP may already
  // name this private stack; only the CRT's pre-dispatch snapshot bounds the
  // still-live parent allocation used by a checked object borrow.
  return State.CxxCatchStacks.begin()->back().SavedStackOffset;
}

bool RegistrationStateSolver::callbackCanReturn(const Domain &State) const {
  if (!KnownCxx)
    return State.Callback && !State.Parent &&
           State.Frame.callbackStackIsRestored(0);
  if (!Chain.hasCxxCallbackStack())
    return true;
  if (State.CxxCatchStacks.size() != 1 ||
      State.CxxCatchStacks.begin()->size() != 1)
    return false;
  const auto &Context = State.CxxCatchStacks.begin()->back();
  const auto Entry = EH.Cxx->TryBlocks[Context.TryIndex]
                         .Handlers[Context.CatchIndex]
                         .HandlerVA;
  const auto &Frame = State.Frame;
  return Frame.CallbackEntry == Entry &&
         Frame.callbackStackIsRestored(*Chain.cxxRuntimeFrameOffset()) &&
         (!Chain.RealignedFrame ||
          Frame.load(Chain.RealignedFrame->SavedParentFrameOffset, 4)
                  .EntryOffset == 0);
}

bool RegistrationStateSolver::callbackReturnInstruction(size_t I,
                                                        const LowOp &Op) const {
  const auto &Block = Function.Blocks[I];
  const auto Boundary = Boundaries.find(Op.Addr);
  if (Op.Opcode != NdOp::RETURN || Op.Seq < 0 || Block.Ops.empty() ||
      &Op != &Block.Ops.back() || !Block.Succs.empty() ||
      Boundary == Boundaries.end() || Boundary->second.first != Block.Id)
    return false;
  const auto &Insn = Boundary->second.second;
  return Insn.Control == LowInstructionControl::Return &&
         !hasLowInstructionControlFlag(
             Insn.ControlFlags, LowInstructionControlFlag::Conditional) &&
         Insn.Immediate.value_or(0) == 0 && Op.Addr < Block.EndAddr &&
         Insn.Size == Block.EndAddr - Op.Addr;
}

} // namespace neverd::registration_state
