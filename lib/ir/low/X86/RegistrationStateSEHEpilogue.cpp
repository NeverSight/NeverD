//===- RegistrationStateSEHEpilogue.cpp - Checked parent fallthrough -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Proves that a finally abandons its own invocation through the complete
/// original parent epilogue, preserving the caller's return and registers.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::proveSEHParentEpilogue(size_t I,
                                                     const FrameState &Frame) {
  const auto &Block = Function.Blocks[I];
  const auto Source = SEHControlIndex.find(Block.StartAddr);
  if (Source == SEHControlIndex.end() ||
      Source->second.Control !=
          RegistrationSEHControlContract::Kind::ParentEpilogue ||
      Source->second.EndAddress != Block.EndAddr ||
      Source->second.Target != Block.EndAddr - 1 ||
      !EH.ownsCode({Block.StartAddr, Block.EndAddr}) || !Block.Succs.empty() ||
      !validateSEHInvocationBlock(I) ||
      !charge(Frame.cellCount() + Frame.OtherRegisterBytes.size() + 10))
    return false;
  FrameState Restored = Frame;
  Restored.leaveCallback();
  FrameTransfer Transfer(Restored, *Chain.RegistrationOffset);
  for (const auto &Op : Block.Ops) {
    if (!charge(1 + Restored.cellCount()) || Op.Seq < 0 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.MemoryOrdering != NdMemoryOrdering::None)
      return false;
    Transfer.beginInstruction(Op.Addr);
    if (Op.Opcode == NdOp::RETURN) {
      if (&Op != &Block.Ops.back() || Op.Addr != Source->second.Target ||
          Restored.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset !=
              4 ||
          !Restored.load(4, 4).ReturnPC)
        return false;
      for (const auto Reg :
           {x86reg::RBX, x86reg::RBP, x86reg::RSI, x86reg::RDI}) {
        const unsigned Index = Reg / x86reg::GeneralRegStride;
        const auto &Value = Restored.Registers[Index];
        if (Value.SavedRegister != Index + 1 || Value.Offset ||
            Value.EntryOffset || Value.CallbackAddress || Value.SEHException)
          return false;
      }
      return true;
    }
    if ((Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::LOAD &&
         Op.Opcode != NdOp::INT_ADD) ||
        Op.Output.Size != 4 ||
        (!Op.Output.isTemp() &&
         (!Op.Output.isReg() ||
          (Op.Output.Offset != x86reg::RSP && Op.Output.Offset != x86reg::RBP &&
           Op.Output.Offset != x86reg::RBX && Op.Output.Offset != x86reg::RSI &&
           Op.Output.Offset != x86reg::RDI &&
           Op.Output.Offset != x86reg::RCX))))
      return false;
    if (Op.Opcode == NdOp::LOAD) {
      const auto Address = Transfer.read(Op.Inputs[0]);
      const auto SP =
          Restored.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
      if (Op.NumInputs != 1 || !Address.Offset || Address.Offset != SP ||
          *Address.Offset > 0)
        return false;
    }
    Transfer.write(Op, Transfer.evaluate(Op, false));
  }
  return false;
}

bool RegistrationStateSolver::transferSEHParentFallthrough(size_t I,
                                                           Domain &After,
                                                           const LowOp &Op,
                                                           BlockFacts &Facts) {
  const auto Refuse = [&] {
    Result.SEHInvocationExitsComplete = false;
    return false;
  };
  const auto &Block = Function.Blocks[I];
  const auto Source = Boundaries.find(Op.Addr);
  const auto &SP =
      After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!SEHControls || KnownCxx || Chain.RealignedFrame || After.Unknown ||
      Facts.Invalid || After.Parent || !After.Callback ||
      After.Activation.SearchFilter || !After.Activation.Entry ||
      !FinallyTargets.count(*After.Activation.Entry) ||
      After.Frame.CallbackEntry != After.Activation.Entry || After.Installed ||
      !After.Uninstalled || !After.StackRestore || !SP.Offset ||
      SP.Offset != After.StackRestore->StackOffset ||
      After.Frame.load(*Chain.RegistrationOffset - 8, 4).Offset != SP.Offset ||
      After.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
          0 ||
      Op.Seq < 0 || Op.Opcode != NdOp::STORE ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS ||
      Op.MemoryOrdering != NdMemoryOrdering::None || Block.Ops.empty() ||
      &Op != &Block.Ops.back() || Source == Boundaries.end() ||
      Source->second.first != Block.Id ||
      Source->second.second.Address + Source->second.second.Size !=
          Block.EndAddr ||
      Source->second.second.Control != LowInstructionControl::None ||
      Source->second.second.ControlFlags != LowInstructionControlFlag::None ||
      Source->second.second.Mode != InstructionMode::Default ||
      Source->second.second.TargetMode != LowInstructionTargetMode::Preserve ||
      Block.Succs.size() != 1 || !Index.count(Block.Succs.front()))
    return Refuse();
  const size_t Target = Index.at(Block.Succs.front());
  const va_t TargetVA = Function.Blocks[Target].StartAddr;
  if (TargetVA != Block.EndAddr || FinallyTargets.count(TargetVA))
    return Refuse();
  for (const auto &Scope : Chain.Scopes)
    if (Scope.HandlerVA == TargetVA || Scope.FilterVA == TargetVA)
      return Refuse();
  if (!proveSEHParentEpilogue(Target, After.Frame))
    return Refuse();
  RegistrationSEHInvocationExit Exit;
  Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentFallthrough;
  Exit.EntryVA = *After.Activation.Entry;
  Exit.CallVA = After.Activation.OrdinaryCall;
  Exit.Address = Op.Addr;
  Exit.EndAddress = Block.EndAddr;
  Exit.OpSeq = Op.Seq;
  Exit.TargetVA = TargetVA;
  Exit.StackRestoreVA = After.StackRestore->Address;
  Exit.StackRestoreSeq = After.StackRestore->Seq;
  Exit.StackOffset = *SP.Offset;
  const SEHExitKey Identity{Exit.EntryVA, Exit.CallVA, Exit.Address,
                            Exit.OpSeq};
  if (InvalidSEHExits.count(Identity) || !charge(After.Frame.cellCount() + 10))
    return Refuse();
  const auto [It, New] = SEHExits.emplace(Identity, Exit);
  if (!New && It->second != Exit) {
    SEHExits.erase(It);
    InvalidSEHExits.insert(Identity);
    return Refuse();
  }
  // Chain removal has already retired the source registration. Keep that
  // state while abandoning every callback stack and entering the epilogue.
  After.Frame.leaveCallback();
  After.SEHCallers.clear();
  After.Activation = {};
  After.OrdinaryCallStack.reset();
  After.StackRestore.reset();
  After.Parent = true;
  After.Callback = After.OtherCallback = false;
  return true;
}

} // namespace neverd::registration_state
