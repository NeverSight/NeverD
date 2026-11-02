//===- RegistrationStateTryLevel.cpp - Checked state writes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds source state stores and preserves untouched bytes of narrow writes.

#include "RegistrationStateSolver.h"
#include "RegistrationTryLevel.h"

#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace neverd::registration_state {

const LowOp *stateStoreCandidate(const RegistrationChainInfo &Chain,
                                 llvm::ArrayRef<LowOp> Instruction) {
  if (!Chain.RegistrationOffset || !Chain.TryLevelOffset ||
      Instruction.empty() || Instruction.size() > 64 ||
      Instruction.back().Opcode != NdOp::STORE)
    return nullptr;
  const unsigned Base =
      Chain.RealignedFrame ? Chain.RealignedFrame->BaseRegister : 5;
  if (Base > 7)
    return nullptr;
  FrameState State;
  State.Registers[Base] = FrameValue::frame(
      Chain.RealignedFrame ? Chain.RealignedFrame->BaseOffset : 0);
  FrameTransfer Transfer(State, *Chain.RegistrationOffset);
  Transfer.beginInstruction(Instruction.front().Addr);
  std::optional<NdVar> Loaded, Updated;
  for (const LowOp &Op : Instruction) {
    if (Op.Addr != Instruction.front().Addr || Op.Seq < 0 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Op.MemoryOrdering != NdMemoryOrdering::None || Op.NumInputs > 2)
      return nullptr;
    for (unsigned I = 0; I != Op.NumInputs; ++I)
      if ((!Op.Inputs[I].isReg() && !Op.Inputs[I].isTemp() &&
           !Op.Inputs[I].isConst()) ||
          (Op.Inputs[I].Size != 1 && Op.Inputs[I].Size != 4 &&
           Op.Inputs[I].Size != 8))
        return nullptr;
    if (Op.Opcode == NdOp::STORE)
      return &Op == &Instruction.back() && Op.NumInputs == 2 &&
                     (Op.Inputs[0].Size == 4 || Op.Inputs[0].Size == 8) &&
                     Op.Inputs[1].Size == 4 &&
                     (Loaded ? Updated && Op.Inputs[1] == *Updated
                             : Op.Inputs[1].isReg() &&
                                   Op.Inputs[1].Offset %
                                           x86reg::GeneralRegStride ==
                                       0 &&
                                   Op.Inputs[1].Offset <
                                       8 * x86reg::GeneralRegStride) &&
                     Transfer.read(Op.Inputs[0]).Offset == Chain.TryLevelOffset
                 ? &Op
                 : nullptr;
    const bool Flag = Op.Output.isReg() && Op.Output.Size == 1 &&
                      Op.Output.Offset >= x86reg::CF &&
                      Op.Output.Offset <= x86reg::OF;
    // The only architectural writes before the final store are arithmetic
    // flags after an exact state-slot load. Neither address registers nor the
    // loaded/updated temporary may be replaced by a later operation.
    if ((!Op.Output.isTemp() && !Flag) || (Flag && !Loaded) ||
        (Op.Output.Size != 1 && Op.Output.Size != 4 && Op.Output.Size != 8) ||
        (Loaded && Op.Output == *Loaded) || (Updated && Op.Output == *Updated))
      return nullptr;
    switch (Op.Opcode) {
    case NdOp::LOAD:
      if (Loaded || !Op.Output.isTemp() || Op.Output.Size != 4 ||
          Op.NumInputs != 1 ||
          (Op.Inputs[0].Size != 4 && Op.Inputs[0].Size != 8) ||
          Transfer.read(Op.Inputs[0]).Offset != Chain.TryLevelOffset)
        return nullptr;
      Loaded = Op.Output;
      break;
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      if (Op.NumInputs != 1 ||
          (Op.Opcode == NdOp::COPY
               ? Op.Output.Size != Op.Inputs[0].Size
               : Flag || Op.Output.Size < Op.Inputs[0].Size))
        return nullptr;
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Flag || Op.Output.Size == 1 || Op.NumInputs != 2 ||
          Op.Inputs[0].Size != Op.Output.Size ||
          Op.Inputs[1].Size != Op.Output.Size)
        return nullptr;
      break;
    case NdOp::SUBBYTES:
      if (Flag || Op.NumInputs != 2 ||
          (Op.Output.Size != 1 && Op.Output.Size != 4) ||
          (Op.Inputs[0].Size != 4 && Op.Inputs[0].Size != 8) ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Offset != 0)
        return nullptr;
      break;
    case NdOp::INT_AND:
    case NdOp::INT_OR:
      if (!Loaded || Flag || Op.NumInputs != 2 ||
          Op.Inputs[0].Size != Op.Output.Size ||
          Op.Inputs[1].Size != Op.Output.Size)
        return nullptr;
      if (Op.Output.Size != 1) {
        if (Updated || Op.Output.Size != 4 ||
            (Op.Inputs[0] != *Loaded && Op.Inputs[1] != *Loaded))
          return nullptr;
        Updated = Op.Output;
      }
      break;
    case NdOp::INT_EQUAL:
    case NdOp::INT_SLESS:
      if (!Loaded || Op.NumInputs != 2 || Op.Output.Size != 1 ||
          Op.Inputs[0].Size != Op.Inputs[1].Size)
        return nullptr;
      break;
    case NdOp::POPCOUNT:
      if (!Loaded || Flag || Op.NumInputs != 1 || Op.Output.Size != 1 ||
          Op.Inputs[0].Size != 1)
        return nullptr;
      break;
    default:
      return nullptr;
    }
    Transfer.write(Op, Transfer.evaluate(Op, false));
  }
  return nullptr;
}

bool RegistrationStateSolver::transferTryLevelStore(
    size_t I, const LowOp &Op, const FrameValue &Address,
    const FrameValue &Stored, const std::set<int32_t> &Levels, bool Unknown,
    std::set<int32_t> &WrittenLevels) {
  const auto Refuse = [&] {
    if (!KnownCxx)
      Result.SEHStateStoresComplete = false;
    return false;
  };
  const auto Observation = Stores.find(Op.Addr);
  const uint16_t Width = Op.NumInputs == 2 ? Op.Inputs[1].Size : 0;
  if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Address.Offset != Chain.TryLevelOffset || Address.EntryOffset ||
      Address.CallbackAddress || (Width != 1 && Width != 2 && Width != 4) ||
      !Stored.Constant)
    return Refuse();
  if (Observation != Stores.end()) {
    if (Observation->second.Width != Width ||
        uint32_t(Observation->second.Level) != *Stored.Constant)
      return Refuse();
  } else {
    // A register's definition can precede its state write by many
    // instructions. Both C++ and SEH use the decoded instruction and complete
    // reaching frame/value transfer; a loader literal is not this proof.
    const auto Boundary = Boundaries.find(Op.Addr);
    const auto &Block = Function.Blocks[I];
    if (Boundary == Boundaries.end() || Boundary->second.first != Block.Id)
      return Refuse();
    const auto &Instruction = Boundary->second.second;
    if (Instruction.Address != Op.Addr || Instruction.Size == 0 ||
        Op.Addr >= Block.EndAddr ||
        Instruction.Size != Block.EndAddr - Op.Addr ||
        Instruction.Control != LowInstructionControl::None ||
        Instruction.ControlFlags != LowInstructionControlFlag::None ||
        Instruction.Mode != InstructionMode::Default ||
        Instruction.TargetMode != LowInstructionTargetMode::Preserve ||
        Instruction.Immediate || !Instruction.OpCount ||
        Instruction.FirstOp > Block.Ops.size() ||
        Instruction.OpCount != Block.Ops.size() - Instruction.FirstOp ||
        !charge(Instruction.OpCount))
      return Refuse();
    const auto Ops = llvm::ArrayRef<LowOp>(Block.Ops).slice(
        Instruction.FirstOp, Instruction.OpCount);
    if (stateStoreCandidate(Chain, Ops) != &Op)
      return Refuse();
  }

  std::vector<std::pair<int32_t, int32_t>> Transitions;
  if (Width == 4) {
    if (!validLevel(int32_t(*Stored.Constant)))
      return Refuse();
    WrittenLevels.insert(int32_t(*Stored.Constant));
    if (!Unknown)
      for (int32_t Level : Levels) {
        if (!charge(1))
          return false;
        Transitions.emplace_back(Level, int32_t(*Stored.Constant));
      }
  } else {
    const uint32_t Mask = Width == 1 ? UINT8_MAX : UINT16_MAX;
    if (Unknown || Levels.empty() || *Stored.Constant > Mask)
      return Refuse();
    for (int32_t Level : Levels) {
      if (!charge(1))
        return false;
      const int32_t Written =
          int32_t((uint32_t(Level) & ~Mask) | (*Stored.Constant & Mask));
      if (!validLevel(Written))
        return Refuse();
      WrittenLevels.insert(Written);
      Transitions.emplace_back(Level, Written);
    }
  }
  if (KnownCxx)
    return true;

  const auto Boundary = Boundaries.find(Op.Addr);
  const auto &Block = Function.Blocks[I];
  const size_t OpIndex = &Op - Block.Ops.data();
  // State analysis already checked the decoded extent and the actual lifted
  // address/value. An absent operation index cannot authorize native writes,
  // but does not invalidate that separate semantic transfer.
  if (Boundary == Boundaries.end()) {
    Result.SEHStateStoresComplete = false;
    return true;
  }
  if (Boundary->second.first != Block.Id ||
      Boundary->second.second.Address != Op.Addr ||
      Boundary->second.second.Size > UINT64_MAX - Op.Addr ||
      (Observation != Stores.end() &&
       Boundary->second.second.Address + Boundary->second.second.Size !=
           Observation->second.EndVA))
    return Refuse();
  if (Op.Seq < 0 || !Boundary->second.second.OpCount) {
    Result.SEHStateStoresComplete = false;
    return true;
  }
  if (Boundary->second.second.FirstOp > OpIndex ||
      OpIndex - Boundary->second.second.FirstOp >=
          Boundary->second.second.OpCount ||
      Boundary->second.second.OpCount >
          Block.Ops.size() - Boundary->second.second.FirstOp)
    return Refuse();
  // A full overwrite may repair an unknown source state, but cannot publish
  // native transition authority for the unknown preceding invocation.
  if (Transitions.empty()) {
    Result.SEHStateStoresComplete = false;
    return true;
  }
  RegistrationSEHStateStore Receipt{
      Block.Id, Op.Addr,        Op.Addr + Boundary->second.second.Size,
      Op.Seq,   uint8_t(Width), *Stored.Constant,
      {}};
  const auto [Found, New] =
      SEHStateStores.emplace(std::make_pair(Op.Addr, Op.Seq), Receipt);
  if (!New) {
    Receipt.Transitions = Found->second.Transitions;
    if (Receipt != Found->second)
      return Refuse();
  }
  auto &Recorded = Found->second.Transitions;
  if (!charge(Recorded.size() + Transitions.size()))
    return false;
  Recorded.insert(Recorded.end(), Transitions.begin(), Transitions.end());
  std::sort(Recorded.begin(), Recorded.end());
  Recorded.erase(std::unique(Recorded.begin(), Recorded.end()), Recorded.end());
  return true;
}

bool RegistrationStateSolver::mergeSEHFrameOutcome(SEHFrameOutcomes &Outcomes,
                                                   FrameState State) {
  const auto Level = State.load(Chain.TryLevelOffset, 4).Constant;
  if (!Level || !validLevel(int32_t(*Level)) ||
      !charge(State.cellCount() + State.OtherRegisterBytes.size() + 10))
    return false;
  const auto [It, New] = Outcomes.emplace(int32_t(*Level), State);
  if (!New)
    It->second.merge(State);
  return true;
}

} // namespace neverd::registration_state
