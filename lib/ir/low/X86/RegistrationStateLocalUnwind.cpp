//===- RegistrationStateLocalUnwind.cpp - checked SEH local unwinds ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86Regs.h"

#include <deque>

namespace neverd::registration_state {

bool RegistrationStateSolver::runLocalFinally(va_t Target, FrameState &Parent) {
  const auto Entry = Entries.find(Target);
  const auto ParentSP = Parent.load(*Chain.RegistrationOffset - 8, 4).Offset;
  if (Entry == Entries.end() || !ParentSP ||
      *ParentSP >= *Chain.RegistrationOffset - 8 ||
      !charge(Parent.cellCount() + 1))
    return false;
  FrameState Initial;
  Initial.Cells = Parent.Cells;
  for (auto &Register : Initial.Registers)
    Register.MayBeFrame = true;
  Initial.OtherRegistersMayBeFrame = true;
  Initial.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
      FrameValue::frame(0);
  Initial.enterCallback(Target);
  std::map<size_t, FrameState> Incoming{{Entry->second, Initial}};
  std::deque<size_t> Pending{Entry->second};
  std::set<size_t> Queued{Entry->second};
  std::optional<FrameState> Returned;
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    Queued.erase(I);
    FrameState State = Incoming.at(I);
    FrameTransfer Transfer(State, *Chain.RegistrationOffset);
    const auto &Block = Function.Blocks[I];
    bool HasReturn = false;
    for (const auto &Op : Block.Ops) {
      if (!charge(1 + State.cellCount() + Op.Output.Size) ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        return false;
      const auto Source = Boundaries.find(Op.Addr);
      const size_t OpIndex = &Op - Block.Ops.data();
      if (Op.Seq < 0 || Source == Boundaries.end() ||
          Source->second.first != Block.Id ||
          Source->second.second.FirstOp > OpIndex ||
          Source->second.second.OpCount <=
              OpIndex - Source->second.second.FirstOp ||
          Source->second.second.FirstOp > Block.Ops.size() ||
          Source->second.second.OpCount >
              Block.Ops.size() - Source->second.second.FirstOp)
        return false;
      Transfer.beginInstruction(Op.Addr);
      if (Op.Opcode == NdOp::RETURN) {
        if (!callbackReturnInstruction(I, Op) ||
            State.CallbackEntry != Target || !State.callbackStackIsRestored(0))
          return false;
        if (!Returned)
          Returned = State;
        else if (!charge(Returned->cellCount() + State.cellCount() + 10))
          return false;
        else
          Returned->merge(State);
        HasReturn = true;
        break;
      }
      const auto Memory = lowMemoryOperands(Op);
      if (Memory.Address) {
        if (!Memory.Complete ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE))
          return false;
        const auto Address = Transfer.read(*Memory.Address);
        if (Address.CallbackAddress) {
          if (!charge(Memory.AccessSize) ||
              !State.callbackMemoryIsPrivate(*Address.CallbackAddress,
                                             Memory.AccessSize,
                                             Op.Opcode == NdOp::LOAD))
            return false;
          if (Op.Opcode == NdOp::STORE)
            State.storeCallback(Address.CallbackAddress->Offset,
                                Memory.AccessSize,
                                Transfer.read(*Memory.StoredValue));
        } else if (!Address.Offset || *Address.Offset < *ParentSP ||
                   int64_t(*Address.Offset) + Memory.AccessSize > 0) {
          return false;
        } else if (Op.Opcode == NdOp::STORE) {
          // The runtime owns SavedESP, exception pointers and the complete
          // registration record. A finally that can change them needs a
          // separate nonlocal-transition proof, not an ordinary return fact.
          if (int64_t(*Address.Offset) + Memory.AccessSize >
              int64_t(*Chain.RegistrationOffset) - 8)
            return false;
          const auto Value = Transfer.read(*Memory.StoredValue);
          if (Value.CallbackAddress || (Value.MayBeFrame && !Value.Offset))
            return false;
          State.store(*Address.Offset, Memory.AccessSize, Value);
        }
      }
      Transfer.write(Op, Transfer.evaluate(Op, true));
      if (Op.Output.isReg() && Op.Output.Offset < x86reg::RSP + 4 &&
          x86reg::RSP < Op.Output.Offset + Op.Output.Size) {
        const auto SP = State.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                            .CallbackAddress;
        if (!SP || SP->Entry != Target || SP->Offset > 0 ||
            !charge(State.CallbackCells.size() +
                    State.InitializedCallbackBytes.size()))
          return false;
        State.trimCallbackCells();
      }
    }
    if (HasReturn) {
      if (!Block.Succs.empty())
        return false;
      continue;
    }
    if (Block.Succs.empty())
      return false;
    for (int Next : Block.Succs) {
      const auto Successor = Index.find(Next);
      if (Successor == Index.end() || !charge(State.cellCount() + 10))
        return false;
      auto [It, New] = Incoming.emplace(Successor->second, State);
      if ((New || It->second.merge(State)) &&
          Queued.insert(Successor->second).second)
        Pending.push_back(Successor->second);
    }
  }
  if (!Returned)
    return false;
  Parent.Cells = std::move(Returned->Cells);
  return true;
}

std::optional<FrameValue>
RegistrationStateSolver::transferLocalUnwind(size_t I, Domain &After,
                                             const LowOp &Op) {
  if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
      Op.NumInputs != 1 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      !LocalUnwindTargets.count(
          {Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL}))
    return std::nullopt;
  auto Refuse = [&]() -> std::optional<FrameValue> {
    FailedLocalUnwind = Facts[I].Invalid = After.Unknown = true;
    After.Levels.clear();
    return std::nullopt;
  };
  const auto &Block = Function.Blocks[I];
  const auto Boundary = Boundaries.find(Op.Addr);
  const auto SP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!Function.hasCompleteLiftCoverage() || KnownCxx || EH4 ||
      EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != -1 ||
      !After.Installed || After.Uninstalled || After.Unknown ||
      Facts[I].Invalid || !SP.Offset || *SP.Offset > INT32_MAX - 4 ||
      Op.Seq < 0 || Boundary == Boundaries.end() ||
      Boundary->second.first != Block.Id ||
      Boundary->second.second.Control != LowInstructionControl::Call ||
      hasLowInstructionControlFlag(Boundary->second.second.ControlFlags,
                                   LowInstructionControlFlag::Conditional) ||
      Boundary->second.second.Address + Boundary->second.second.Size !=
          Block.EndAddr ||
      After.Frame.load(*SP.Offset, 4).Offset != Chain.RegistrationOffset)
    return Refuse();
  const auto &Insn = Boundary->second.second;
  const size_t OpIndex = &Op - Block.Ops.data();
  if (Insn.FirstOp > OpIndex || OpIndex - Insn.FirstOp >= Insn.OpCount ||
      Insn.FirstOp > Block.Ops.size() ||
      Insn.OpCount > Block.Ops.size() - Insn.FirstOp)
    return Refuse();
  const auto Level = After.Frame.load(*SP.Offset + 4, 4).Constant;
  if (!Level || !validLevel(int32_t(*Level)) || After.Levels.empty())
    return Refuse();
  std::optional<FrameState> Joined;
  for (int32_t From : After.Levels) {
    if (!charge(After.Frame.cellCount() + 1))
      return Refuse();
    FrameState Frame = After.Frame;
    int32_t Walk = From;
    size_t Steps = 0;
    while (Walk != int32_t(*Level)) {
      if (!charge(1) || Walk < 0 || size_t(Walk) >= Chain.Scopes.size() ||
          ++Steps > Chain.Scopes.size())
        return Refuse();
      const auto &Scope = Chain.Scopes[Walk];
      Walk = Scope.EnclosingLevel;
      Frame.store(*Chain.TryLevelOffset, 4,
                  FrameValue::constant(uint32_t(Walk)));
      if (Scope.IsFinally && !runLocalFinally(Scope.HandlerVA, Frame))
        return Refuse();
    }
    if (!Joined)
      Joined = std::move(Frame);
    else if (!charge(Joined->cellCount() + Frame.cellCount() + 10))
      return Refuse();
    else
      Joined->merge(Frame);
  }
  After.Frame.Cells = std::move(Joined->Cells);
  After.Levels = {int32_t(*Level)};
  After.Frame.store(*Chain.TryLevelOffset, 4, FrameValue::constant(*Level));
  // _local_unwind2 is caller-cleanup. Its proved returning path retains the
  // argument-area ESP, independently of every memory/native-output contract.
  return SP;
}

} // namespace neverd::registration_state
