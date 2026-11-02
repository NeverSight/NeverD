//===- RegistrationStateSEHThread.cpp - PE32 thread read receipts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep current-thread memory separate from the exception chain and frames.
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::recordSEHThreadMemory(
    size_t I, Domain &After, const LowOp &Op, const FrameTransfer &Transfer,
    BlockFacts &Facts) {
  using Thread = RegistrationSEHThreadAccess;
  const auto Memory = lowMemoryOperands(Op);
  if (KnownCxx || Conditional || !Memory.Address || !charge(1))
    return false;
  const auto Address = Transfer.read(*Memory.Address);
  const bool Self = Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
                    Address.Constant == Thread::SelfOffset;
  std::optional<Thread> Access;
  if (Self || Address.ThreadAddress) {
    const auto Boundary = Boundaries.find(Op.Addr);
    bool Valid = After.Parent && !After.Callback && After.Installed &&
                 !After.Uninstalled && !After.Unknown && !Facts.Invalid &&
                 Memory.Complete && Op.Opcode == NdOp::LOAD && Op.Seq >= 0 &&
                 Thread::isAddressWidth(Memory.Address->Size) &&
                 Op.Output.Size == Thread::WordBytes &&
                 Op.MemoryOrdering == NdMemoryOrdering::None &&
                 Boundary != Boundaries.end() &&
                 Boundary->second.first == Function.Blocks[I].Id;
    if (Valid) {
      const auto &Insn = Boundary->second.second;
      const auto &Block = Function.Blocks[I];
      const size_t OpIndex = &Op - Block.Ops.data();
      Valid =
          Insn.FirstOp <= Block.Ops.size() &&
          Insn.OpCount <= Block.Ops.size() - Insn.FirstOp &&
          OpIndex >= Insn.FirstOp && OpIndex - Insn.FirstOp < Insn.OpCount &&
          Insn.Mode == InstructionMode::Default &&
          !hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::Conditional) &&
          !hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard);
    }
    if (Self) {
      if (Valid)
        Access = {Thread::Kind::Self, Op.Addr, Op.Seq, Op.Addr, Op.Seq};
    } else {
      Valid &= Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
               Address.ThreadAddress->Offset == Thread::StackBaseOffset;
      if (Valid)
        Access = {Thread::Kind::StackBase, Op.Addr, Op.Seq,
                  Address.ThreadAddress->RootAddress,
                  Address.ThreadAddress->RootOpSeq};
    }
    if (!Valid) {
      Result.SEHThreadAccessesComplete = CompleteImageReads = false;
      Facts.Invalid = After.Unknown = true;
      After.Levels.clear();
    }
  }
  const auto [It, New] =
      SEHThreadAccesses.emplace(std::make_pair(Op.Addr, Op.Seq), Access);
  if (!New && It->second != Access) {
    Result.SEHThreadAccessesComplete = CompleteImageReads = false;
    It->second.reset();
  }
  return Access.has_value();
}

} // namespace neverd::registration_state
