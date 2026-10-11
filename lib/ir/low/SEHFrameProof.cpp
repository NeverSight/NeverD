//===- SEHFrameProof.cpp - Local-unwind frame arguments -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Tracks entry-relative stack identities through the complete LowIR CFG.

#include "neverd/ir/low/SEHFrameProof.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowUndefinedEffects.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }

struct State {
  std::map<Key, int64_t> Values;
  std::map<int64_t, int64_t> Spills;
  bool operator==(const State &) const = default;

  std::optional<int64_t> get(const NdVar &V) const {
    if (V.Size != 8 || (!V.isReg() && !V.isTemp()))
      return std::nullopt;
    const auto It = Values.find(key(V));
    return It == Values.end() ? std::nullopt
                              : std::optional<int64_t>{It->second};
  }

  void dropTemporaries() {
    std::erase_if(Values, [](const auto &V) {
      return std::get<0>(V.first) == VnodeSpace::TEMP;
    });
  }

  void intersect(const State &Other) {
    const auto Missing = [](const auto &Map, const auto &Item) {
      const auto It = Map.find(Item.first);
      return It == Map.end() || It->second != Item.second;
    };
    std::erase_if(Values,
                  [&](const auto &V) { return Missing(Other.Values, V); });
    std::erase_if(Spills,
                  [&](const auto &V) { return Missing(Other.Spills, V); });
  }

  void invalidateMemory(int64_t Start, int64_t End) {
    std::erase_if(Spills, [&](const auto &S) {
      int64_t Limit = 0;
      return llvm::AddOverflow(S.first, int64_t{8}, Limit) ||
             (Start < Limit && S.first < End);
    });
  }
};

bool consume(size_t &Remaining, size_t Amount = 1) {
  if (Amount > Remaining)
    return false;
  Remaining -= Amount;
  return true;
}

bool transfer(const LowBlock &Block, State &S, const TargetRegInfo &TRI,
              size_t &Remaining, LowSEHFrameProof *Record) {
  const NdVar SP = NdVar::reg(TRI.StackPointer, 8);
  const NdVar Frame = NdVar::reg(TRI.Win64ParamRegs.front(), 8);
  for (const auto &Boundary : Block.InstructionBoundaries) {
    if (!consume(Remaining))
      return false;
    if (hasLowInstructionControlFlag(
            Boundary.ControlFlags,
            LowInstructionControlFlag::InstructionGuard) ||
        Boundary.Control == LowInstructionControl::ConditionalCall ||
        Boundary.Control == LowInstructionControl::ConditionalReturn) {
      S = {};
      return true;
    }
  }
  va_t Instruction = InvalidVA;
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    if (!consume(Remaining))
      return false;
    const LowOp &Op = Block.Ops[I];
    if (Op.Opcode >= NdOp::_COUNT || Op.NumInputs > std::size(Op.Inputs) ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Op.Output.Size && ((!Op.Output.isReg() && !Op.Output.isTemp()) ||
                            Op.Output.Offset > UINT64_MAX - Op.Output.Size))) {
      S = {};
      return true;
    }
    if (Op.Addr != Instruction) {
      S.dropTemporaries();
      Instruction = Op.Addr;
    }
    if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
        Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN) {
      // An instruction-local branch cannot be flattened into unconditional
      // copies. The ordinary terminal branch's alternatives already join in
      // the CFG. A malformed trailing effect supplies no outgoing facts.
      if (I + 1 != Block.Ops.size() || Op.Output.Size)
        S = {};
      break;
    }
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        Op.Opcode == NdOp::INTRINSIC) {
      const auto CurrentSP = S.get(SP);
      if (Record && CurrentSP && S.get(Frame) == CurrentSP &&
          Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8) {
        if (!Record->Calls.emplace(std::make_pair(Op.Addr, Op.Seq), *CurrentSP)
                 .second)
          return false;
      }
      // A pointer to any neighbouring local may let an opaque callee write
      // the saved SP. Pointer width does not bound the pointee's extent.
      S = {};
      const bool DefinesSP = Op.Output.isReg() && Op.Output.Size &&
                             Op.Output.Offset < SP.Offset + SP.Size &&
                             SP.Offset < Op.Output.Offset + Op.Output.Size;
      if (CurrentSP && Op.Opcode != NdOp::INTRINSIC && !DefinesSP)
        S.Values[key(SP)] = *CurrentSP;
      continue;
    }
    if (Op.Opcode == NdOp::STORE) {
      if (Op.NumInputs != 2 || Op.Output.Size) {
        S = {};
        return true;
      }
      const auto Address = S.get(Op.Inputs[0]);
      const auto Value = S.get(Op.Inputs[1]);
      int64_t End = 0;
      if (!Address || !Op.Inputs[1].Size ||
          llvm::AddOverflow(*Address, int64_t{Op.Inputs[1].Size}, End)) {
        S.Spills.clear();
        continue;
      }
      S.invalidateMemory(*Address, End);
      if (Value && Op.Inputs[1].Size == 8)
        S.Spills[*Address] = *Value;
      continue;
    }
    if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
        Op.Opcode == NdOp::ATOMIC_CMPXCHG)
      S.Spills.clear();
    std::optional<int64_t> Value;
    if (Op.Output.Size == 8 && Op.NumInputs == 1 && Op.Opcode == NdOp::COPY)
      Value = S.get(Op.Inputs[0]);
    else if (Op.Output.Size == 8 && Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
             Op.Inputs[1].isConst() && Op.Inputs[1].Size == 8) {
      if (auto Base = S.get(Op.Inputs[0])) {
        const int64_t Delta = static_cast<int64_t>(Op.Inputs[1].Offset);
        int64_t Sum = 0;
        const bool Overflow = Op.Opcode == NdOp::INT_ADD
                                  ? llvm::AddOverflow(*Base, Delta, Sum)
                                  : llvm::SubOverflow(*Base, Delta, Sum);
        if (!Overflow)
          Value = Sum;
      }
    } else if (Op.Output.Size == 8 && Op.NumInputs == 1 &&
               Op.Opcode == NdOp::LOAD) {
      if (auto Address = S.get(Op.Inputs[0]))
        if (auto It = S.Spills.find(*Address); It != S.Spills.end())
          Value = It->second;
    }
    if (!Op.Output.Size)
      continue;
    if (Op.Output.Space == SP.Space && Op.Output.Offset < SP.Offset + SP.Size &&
        SP.Offset < Op.Output.Offset + Op.Output.Size) {
      const auto Before = S.get(SP);
      if (!Value || !Before || Op.Output != SP)
        S.Spills.clear();
      else if (*Before < *Value)
        S.invalidateMemory(*Before, *Value);
    }
    std::erase_if(S.Values, [&](const auto &V) {
      const auto &[Space, Offset, Size] = V.first;
      return Space == Op.Output.Space &&
             Offset < Op.Output.Offset + Op.Output.Size &&
             Op.Output.Offset < Offset + Size;
    });
    if (Value)
      S.Values[key(Op.Output)] = *Value;
  }
  S.dropTemporaries();
  return true;
}

} // namespace

std::string lowSEHFrameDependencyDigest(const LowFunc &F) {
  llvm::SHA256 Hash;
  Hash.update("neverd-low-seh-frame-cfg-v1");
  const auto Word = [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Value >> (8 * I));
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  };
  const auto Words = [&](const auto &Items) {
    Word(Items.size());
    for (const auto &Item : Items)
      Word(Item);
  };
  Word(F.Entry);
  Word(F.hasCompleteLiftCoverage());
  Words(F.ModuleAnalysisRoots);
  Words(F.OrdinaryModuleAnalysisRoots);
  Word(F.FunctionTemporaries.size());
  for (const auto &Temporary : F.FunctionTemporaries) {
    Word(Temporary.Offset);
    Word(Temporary.Bytes);
  }
  Word(F.Blocks.size());
  for (const auto &Block : F.Blocks) {
    Word(Block.Id);
    Word(Block.StartAddr);
    Word(Block.EndAddr);
    Hash.update(lowUndefinedOperationDigest(Block.Ops));
    Words(Block.Preds);
    Words(Block.Succs);
    Word(Block.ExceptionalPreds.size());
    for (const auto &Edge : Block.ExceptionalPreds)
      Word(Edge.BlockId);
    Word(Block.ExceptionalSuccs.size());
    for (const auto &Edge : Block.ExceptionalSuccs)
      Word(Edge.BlockId);
    Word(Block.InstructionBoundaries.size());
    for (const auto &Boundary : Block.InstructionBoundaries) {
      Word(Boundary.Address);
      Word(Boundary.Size);
      Word(Boundary.FirstOp);
      Word(Boundary.OpCount);
      Word(static_cast<unsigned>(Boundary.Mode));
      Word(static_cast<unsigned>(Boundary.Control));
      Word(static_cast<unsigned>(Boundary.ControlFlags));
      Word(static_cast<unsigned>(Boundary.TargetMode));
      Word(Boundary.Immediate.has_value());
      if (Boundary.Immediate)
        Word(*Boundary.Immediate);
    }
  }
  return llvm::toHex(Hash.final(), true);
}

LowSEHFrameProof proveLowSEHFrames(const LowFunc &F, const TargetRegInfo &TRI,
                                   size_t &Remaining) {
  if (TRI.TheArch != Arch::X64 || TRI.PointerSize != 8 ||
      TRI.Win64ParamRegs.empty() || F.Blocks.empty() ||
      !F.hasCompleteLiftCoverage() || !F.FunctionTemporaries.empty())
    return {};
  const size_t N = F.Blocks.size();
  std::map<int, size_t> Positions;
  std::set<va_t> Starts;
  for (size_t I = 0; I < N; ++I)
    if (!consume(Remaining) || F.Blocks[I].Id < 0 ||
        !Positions.emplace(F.Blocks[I].Id, I).second ||
        !Starts.insert(F.Blocks[I].StartAddr).second)
      return {};
  if (!Starts.count(F.Entry))
    return {};
  for (const auto *Roots :
       {&F.ModuleAnalysisRoots, &F.OrdinaryModuleAnalysisRoots})
    for (va_t Root : *Roots)
      if (!consume(Remaining) || !Starts.count(Root))
        return {};
  std::vector<State> Inputs(N), Outputs(N);
  std::vector<bool> Roots(N), Visited(N), Queued(N);
  std::deque<size_t> Queue;
  const auto Enqueue = [&](size_t I) {
    if (!Queued[I]) {
      Queued[I] = true;
      Queue.push_back(I);
    }
  };
  for (size_t I = 0; I < N; ++I) {
    const auto &Block = F.Blocks[I];
    if (!consume(Remaining, Block.Preds.size() + Block.Succs.size()))
      return {};
    for (int Id : Block.Preds)
      if (!Positions.count(Id) || !F.Blocks[Positions.at(Id)].hasSucc(Block.Id))
        return {};
    for (int Id : Block.Succs)
      if (!Positions.count(Id) ||
          std::find(F.Blocks[Positions.at(Id)].Preds.begin(),
                    F.Blocks[Positions.at(Id)].Preds.end(),
                    Block.Id) == F.Blocks[Positions.at(Id)].Preds.end())
        return {};
    Roots[I] = Block.StartAddr == F.Entry || Block.Preds.empty() ||
               F.ModuleAnalysisRoots.count(Block.StartAddr) ||
               F.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) ||
               !Block.ExceptionalPreds.empty();
    if (Roots[I])
      Enqueue(I);
  }
  while (!Queue.empty()) {
    if (!consume(Remaining))
      return {};
    const size_t I = Queue.front();
    Queue.pop_front();
    Queued[I] = false;
    const auto &Block = F.Blocks[I];
    std::optional<State> Input;
    if (Roots[I]) {
      Input.emplace();
      if (Block.StartAddr == F.Entry && Block.ExceptionalPreds.empty())
        Input->Values[key(NdVar::reg(TRI.StackPointer, 8))] = 0;
    }
    for (int Id : Block.Preds) {
      const size_t P = Positions.at(Id);
      if (!consume(Remaining,
                   Outputs[P].Values.size() + Outputs[P].Spills.size() + 1))
        return {};
      if (!Visited[P])
        continue;
      if (Input)
        Input->intersect(Outputs[P]);
      else
        Input = Outputs[P];
    }
    if (!Input)
      continue;
    State Output = *Input;
    if (!transfer(Block, Output, TRI, Remaining, nullptr))
      return {};
    const bool Changed = !Visited[I] || Output != Outputs[I];
    Visited[I] = true;
    Inputs[I] = std::move(*Input);
    Outputs[I] = std::move(Output);
    if (Changed)
      for (int Id : Block.Succs)
        Enqueue(Positions.at(Id));
  }
  LowSEHFrameProof Result;
  for (size_t I = 0; I < N; ++I)
    if (Visited[I] &&
        !transfer(F.Blocks[I], Inputs[I], TRI, Remaining, &Result))
      return {};
  if (!Result.Calls.empty()) {
    for (const auto &Block : F.Blocks)
      if (!consume(Remaining, Block.Ops.size() +
                                  Block.InstructionBoundaries.size() +
                                  Block.Preds.size() + Block.Succs.size() + 1))
        return {};
    Result.DependencyDigest = lowSEHFrameDependencyDigest(F);
  }
  return Result;
}

} // namespace neverd
