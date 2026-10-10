//===- RegistrationStateCxx.cpp - x86 EH catch transfers ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

void RegistrationStateSolver::recordCatchReturn(
    size_t I, const Domain &After, const LowOp &Op,
    const FrameTransfer &Transfer,
    std::optional<RegistrationCxxContinuation> &CatchReturn) {
  if (!KnownCxx || Op.Opcode != NdOp::RETURN || After.CxxCatchStacks.empty())
    return;
  const LowBlock &Block = Function.Blocks[I];
  const auto Identity = std::make_pair(Op.Addr, Op.Seq);
  const auto Boundary = Boundaries.find(Op.Addr);
  const FrameValue Target =
      Op.NumInputs == 1 ? Transfer.read(Op.Inputs[0]) : FrameValue{};
  const int64_t SavedSlot = int64_t(*Chain.RegistrationOffset) - 4;
  const auto CapturedSP =
      After.CxxCatchStacks.size() == 1
          ? After.CxxCatchStacks.begin()->back().SavedStackOffset
          : std::nullopt;
  // The runtime owns this snapshot, independently of subsequent catch writes
  // to SavedESP. The continuation edge restores both the cell and ESP.
  const bool Valid =
      realignedCatchCanReturn(After) && After.CxxCatchStacks.size() == 1 &&
      !After.Parent && !After.OtherCallback && !After.Unknown &&
      !Facts[I].Invalid && After.Installed && !After.Uninstalled &&
      !After.Levels.empty() && Op.Seq >= 0 && Op.NumInputs == 1 &&
      Op.Inputs[0].Size == 4 && &Op == &Block.Ops.back() &&
      Block.Succs.empty() && Target.Constant && !Target.MayBeFrame &&
      EH.CodeRange.contains(*Target.Constant) &&
      *Target.Constant != Function.Entry && CapturedSP &&
      *CapturedSP <= SavedSlot && Boundary != Boundaries.end() &&
      Boundary->second.first == Block.Id &&
      Boundary->second.second.Control == LowInstructionControl::Return &&
      Boundary->second.second.Immediate.value_or(0) == 0 &&
      Op.Addr + Boundary->second.second.Size == Block.EndAddr;
  if (!Valid) {
    InvalidCxxContinuations.insert(Identity);
    CxxContinuations.erase(Identity);
    CompleteCxxContinuations = false;
  } else if (!InvalidCxxContinuations.count(Identity)) {
    const auto &Context = After.CxxCatchStacks.begin()->back();
    CatchReturn = RegistrationCxxContinuation{
        Context.TryIndex, Context.CatchIndex, Op.Addr,    Block.EndAddr,
        Op.Seq,           *Target.Constant,   *CapturedSP};
    const auto [It, Inserted] =
        CxxContinuations.emplace(Identity, *CatchReturn);
    if (!Inserted && It->second != *CatchReturn) {
      InvalidCxxContinuations.insert(Identity);
      CxxContinuations.erase(Identity);
      CatchReturn.reset();
      CompleteCxxContinuations = false;
    }
  }
}

bool RegistrationStateSolver::recordRuntimeMemory(
    size_t I, Domain &After, const LowOp &Op, const FrameTransfer &Transfer,
    const FrameTransfer &RuntimeTransfer) {
  const auto Memory = lowMemoryOperands(Op);
  if (!CheckRuntimeObjects || !Memory.Address)
    return false;
  const LowBlock &Block = Function.Blocks[I];
  bool RuntimeMemory = false;
  const auto RuntimeAddress = RuntimeTransfer.read(*Memory.Address);
  if (RuntimeAddress.MayBeFrame) {
    const auto Identity = std::make_pair(Op.Addr, Op.Seq);
    const auto Boundary = Boundaries.find(Op.Addr);
    bool Valid = Memory.Complete &&
                 Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
                 (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
                 !After.Unknown && !Facts[I].Invalid && After.RuntimeIdentity &&
                 RuntimeAddress.Offset && *RuntimeAddress.Offset >= 0 &&
                 Op.Seq >= 0 && Boundary != Boundaries.end() &&
                 Boundary->second.first == Block.Id &&
                 uint64_t(*RuntimeAddress.Offset) + Memory.AccessSize <=
                     After.RuntimeIdentity->ObjectSize;
    if (Valid && Op.Opcode == NdOp::STORE)
      Valid = !Transfer.read(*Memory.StoredValue).MayBeFrame &&
              !RuntimeTransfer.read(*Memory.StoredValue).MayBeFrame;
    RegistrationRuntimeObjectAccess Access;
    if (Valid) {
      const auto &Object = *After.RuntimeIdentity;
      Access = {Op.Addr,
                Op.Seq,
                Object.TryIndex,
                Object.CatchIndex,
                *RuntimeAddress.Offset,
                Memory.AccessSize,
                Op.Opcode == NdOp::STORE};
      auto [It, New] = RuntimeAccesses.emplace(Identity, Access);
      Valid = (New || It->second == Access) &&
              !InvalidRuntimeAccesses.count(Identity);
    }
    if (!Valid) {
      InvalidRuntimeAccesses.insert(Identity);
      RuntimeAccesses.erase(Identity);
      CompleteRuntimeObjects = CompleteImageReads = false;
    }
    RuntimeMemory = Valid;
  }
  if (Op.Opcode == NdOp::STORE && Memory.StoredValue) {
    const auto Stored = RuntimeTransfer.read(*Memory.StoredValue);
    const auto PrivateAddress = Transfer.read(*Memory.Address);
    if (PrivateAddress.Offset && !RuntimeAddress.MayBeFrame) {
      if (!charge(After.RuntimeObject.cellCount() +
                  (Memory.AccessSize + 3) / 4 + 1))
        return false;
      After.RuntimeObject.store(*PrivateAddress.Offset, Memory.AccessSize,
                                Stored);
    } else if (Stored.MayBeFrame) {
      CompleteRuntimeObjects = false;
    }
  }
  return RuntimeMemory;
}

} // namespace neverd::registration_state
