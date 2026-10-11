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

#include "llvm/ADT/STLExtras.h"
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

struct CallContext {
  const std::map<va_t, const LowFunc *> *Callees = nullptr;
  std::set<va_t> Dependencies;
};

std::optional<State> transferLeaf(const LowFunc &Callee, const State &Caller,
                                  const TargetRegInfo &TRI, size_t &Remaining);

bool transfer(const LowBlock &Block, State &S, const TargetRegInfo &TRI,
              size_t &Remaining, LowSEHFrameProof *Record,
              CallContext *Calls = nullptr,
              std::optional<int64_t> ReturnSlot = std::nullopt) {
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
    if (!consume(Remaining, S.Values.size() + S.Spills.size() + 1))
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
      if (ReturnSlot)
        return false; // This bounded proof admits returning leaves only.
      const auto CurrentSP = S.get(SP);
      if (Record && CurrentSP && S.get(Frame) == CurrentSP &&
          Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8) {
        if (!Record->Calls.emplace(std::make_pair(Op.Addr, Op.Seq), *CurrentSP)
                 .second)
          return false;
      }
      std::optional<State> Returned;
      if (Calls && Calls->Callees && CurrentSP && !S.Spills.empty() &&
          Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8) {
        const auto It = Calls->Callees->find(Op.Inputs[0].Offset);
        if (It != Calls->Callees->end() && It->second &&
            It->second->Entry == It->first) {
          Returned = transferLeaf(*It->second, S, TRI, Remaining);
          if (Returned)
            Calls->Dependencies.insert(It->first);
        }
      }
      // Only the inspected body's actual stores may preserve a saved slot.
      // A neighbouring pointer passed to an opaque callee has no extent.
      S = Returned ? std::move(*Returned) : State{};
      S.Values.clear();
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
        if (ReturnSlot)
          return false; // It may overwrite this invocation's return PC.
        S.Spills.clear();
        continue;
      }
      if (ReturnSlot && *Address < *ReturnSlot + 8 && *ReturnSlot < End)
        return false;
      S.invalidateMemory(*Address, End);
      if (Value && Op.Inputs[1].Size == 8)
        S.Spills[*Address] = *Value;
      continue;
    }
    if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
        Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
      if (ReturnSlot)
        return false;
      S.Spills.clear();
    }
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

// Inspect the concrete argument/frame values at this call, preserving only
// saved cells common to all returns. The architectural call owns one new PC
// slot; LowIR's RETURN does not otherwise expose its load. A store through an
// unknown address, a modified PC, a non-returning exit or a nested call cannot
// establish this contract. No parameter width or ABI declaration bounds writes.
std::optional<State> transferLeaf(const LowFunc &F, const State &Caller,
                                  const TargetRegInfo &TRI, size_t &Remaining) {
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  const auto CallerSP = Caller.get(SP);
  int64_t Slot = 0;
  if (!CallerSP || llvm::SubOverflow(*CallerSP, int64_t{8}, Slot) ||
      !F.hasCompleteLiftCoverage() || !F.FunctionTemporaries.empty() ||
      F.Blocks.empty() || F.CalleePopBytes ||
      (F.ExceptionMetadata &&
       (F.ExceptionMetadata->SEH || F.ExceptionMetadata->Cxx ||
        F.ExceptionMetadata->Registration)) ||
      !consume(Remaining, Caller.Values.size() + Caller.Spills.size() + 1))
    return std::nullopt;
  std::map<int, size_t> Positions;
  std::set<va_t> Starts;
  std::optional<size_t> Entry;
  for (size_t I = 0; I != F.Blocks.size(); ++I) {
    const auto &B = F.Blocks[I];
    if (!consume(Remaining, B.Preds.size() + B.Succs.size() + B.Ops.size() +
                                B.InstructionBoundaries.size() + 1) ||
        B.Id < 0 || !Positions.emplace(B.Id, I).second ||
        !Starts.insert(B.StartAddr).second || !B.ExceptionalPreds.empty() ||
        !B.ExceptionalSuccs.empty())
      return std::nullopt;
    if (auto Error = validateLowInstructionBoundaries(
            B, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return std::nullopt;
    }
    if (B.StartAddr == F.Entry)
      Entry = I;
  }
  if (!Entry || !F.Blocks[*Entry].Preds.empty())
    return std::nullopt;
  for (const auto *Roots :
       {&F.ModuleAnalysisRoots, &F.OrdinaryModuleAnalysisRoots})
    for (va_t Root : *Roots)
      if (!consume(Remaining) || Root != F.Entry)
        return std::nullopt;
  for (const auto &B : F.Blocks) {
    for (int Id : B.Preds)
      if (!Positions.count(Id) || !F.Blocks[Positions.at(Id)].hasSucc(B.Id))
        return std::nullopt;
    std::set<va_t> Successors;
    for (int Id : B.Succs)
      if (!Positions.count(Id) ||
          !llvm::is_contained(F.Blocks[Positions.at(Id)].Preds, B.Id))
        return std::nullopt;
      else
        Successors.insert(F.Blocks[Positions.at(Id)].StartAddr);
    if (B.Ops.empty())
      return std::nullopt;
    const auto &Last = B.Ops.back();
    std::set<va_t> Expected;
    if (Last.Opcode == NdOp::BRANCH || Last.Opcode == NdOp::COND_BR) {
      const bool Conditional = Last.Opcode == NdOp::COND_BR;
      if (Last.NumInputs != (Conditional ? 2 : 1) ||
          !Last.Inputs[0].isConst() || Last.Inputs[0].Size != 8)
        return std::nullopt;
      Expected.insert(Last.Inputs[0].Offset);
      if (Conditional)
        Expected.insert(B.EndAddr);
    } else if (Last.Opcode == NdOp::INDIR_BR)
      return std::nullopt;
    else if (Last.Opcode != NdOp::RETURN)
      Expected.insert(B.EndAddr);
    // Symmetric Preds/Succs alone cannot certify that the CFG includes the
    // decoded instruction's taken and fallthrough paths.
    if (Successors != Expected)
      return std::nullopt;
  }
  State Initial = Caller;
  Initial.dropTemporaries();
  Initial.Values[key(SP)] = Slot;
  // The callee owns new storage below the caller SP, including the pushed PC.
  // Reusing stale values from a previous invocation would prove false loads.
  std::erase_if(Initial.Spills,
                [&](const auto &Cell) { return Cell.first < *CallerSP; });
  std::vector<std::optional<State>> Inputs(F.Blocks.size());
  Inputs[*Entry] = std::move(Initial);
  std::deque<size_t> Queue{*Entry};
  std::vector<bool> Queued(F.Blocks.size());
  Queued[*Entry] = true;
  std::optional<State> Returned;
  while (!Queue.empty()) {
    const size_t I = Queue.front();
    Queue.pop_front();
    Queued[I] = false;
    const auto &B = F.Blocks[I];
    if (!consume(Remaining,
                 Inputs[I]->Values.size() + Inputs[I]->Spills.size() + 1) ||
        B.Ops.empty())
      return std::nullopt;
    State Out = *Inputs[I];
    if (!transfer(B, Out, TRI, Remaining, nullptr, nullptr, Slot))
      return std::nullopt;
    const auto &Last = B.Ops.back();
    if (Last.Opcode == NdOp::RETURN) {
      const auto Boundary =
          llvm::find_if(B.InstructionBoundaries, [&](const auto &Insn) {
            return Insn.Address == Last.Addr;
          });
      if (!B.Succs.empty() || Out.get(SP) != Slot || Last.Seq < 0 ||
          Boundary == B.InstructionBoundaries.end() ||
          Boundary->Control != LowInstructionControl::Return ||
          !hasLowInstructionControlFlag(Boundary->ControlFlags,
                                        LowInstructionControlFlag::Return) ||
          (static_cast<uint16_t>(Boundary->ControlFlags) &
           ~static_cast<uint16_t>(LowInstructionControlFlag::Return |
                                  LowInstructionControlFlag::Indirect)) ||
          Boundary->Immediate.value_or(0) != 0 || !Boundary->OpCount ||
          Boundary->FirstOp >= B.Ops.size() ||
          Boundary->OpCount != B.Ops.size() - Boundary->FirstOp ||
          Last.Addr >= B.EndAddr || Boundary->Size != B.EndAddr - Last.Addr)
        return std::nullopt;
      if (!Returned)
        Returned = Out;
      else {
        if (!consume(Remaining,
                     Returned->Values.size() + Returned->Spills.size()))
          return std::nullopt;
        Returned->intersect(Out);
      }
      continue;
    }
    if (B.Succs.empty() || Last.Opcode == NdOp::INDIR_BR)
      return std::nullopt;
    for (int Id : B.Succs) {
      const size_t Next = Positions.at(Id);
      const size_t OldFacts = Inputs[Next] ? Inputs[Next]->Values.size() +
                                                 Inputs[Next]->Spills.size()
                                           : 0;
      if (!consume(Remaining,
                   Out.Values.size() + Out.Spills.size() + OldFacts + 1))
        return std::nullopt;
      bool Changed = !Inputs[Next];
      if (Changed)
        Inputs[Next] = Out;
      else {
        State Joined = *Inputs[Next];
        Joined.intersect(Out);
        Changed = Joined != *Inputs[Next];
        Inputs[Next] = std::move(Joined);
      }
      if (Changed && !Queued[Next]) {
        Queued[Next] = true;
        Queue.push_back(Next);
      }
    }
  }
  if (Returned)
    std::erase_if(Returned->Spills, [&](const auto &Cell) {
      const auto Before = Caller.Spills.find(Cell.first);
      return Cell.first < *CallerSP || Before == Caller.Spills.end() ||
             Before->second != Cell.second;
    });
  return Returned;
}

} // namespace

std::string lowSEHFrameDependencyDigest(const LowFunc &F) {
  llvm::SHA256 Hash;
  Hash.update("neverd-low-seh-frame-cfg-v2");
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
  Word(F.CalleePopBytes);
  Word(F.ExceptionMetadata &&
       (F.ExceptionMetadata->SEH || F.ExceptionMetadata->Cxx ||
        F.ExceptionMetadata->Registration));
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

LowSEHFrameProof
proveLowSEHFrames(const LowFunc &F, const TargetRegInfo &TRI, size_t &Remaining,
                  const std::map<va_t, const LowFunc *> *Callees) {
  if (TRI.TheArch != Arch::X64 || TRI.PointerSize != 8 ||
      TRI.Win64ParamRegs.empty() || F.Blocks.empty() ||
      !F.hasCompleteLiftCoverage() || !F.FunctionTemporaries.empty())
    return {};
  const size_t N = F.Blocks.size();
  CallContext Calls{Callees, {}};
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
    if (!transfer(Block, Output, TRI, Remaining, nullptr, &Calls))
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
        !transfer(F.Blocks[I], Inputs[I], TRI, Remaining, &Result, &Calls))
      return {};
  if (!Result.Calls.empty()) {
    for (const auto &Block : F.Blocks)
      if (!consume(Remaining, Block.Ops.size() +
                                  Block.InstructionBoundaries.size() +
                                  Block.Preds.size() + Block.Succs.size() + 1))
        return {};
    for (va_t Dependency : Calls.Dependencies)
      for (const auto &Block : Callees->at(Dependency)->Blocks)
        if (!consume(Remaining,
                     Block.Ops.size() + Block.InstructionBoundaries.size() +
                         Block.Preds.size() + Block.Succs.size() + 1))
          return {};
    Result.CalleeDependencies.assign(Calls.Dependencies.begin(),
                                     Calls.Dependencies.end());
    Result.DependencyDigest =
        lowSEHFrameDependencyDigest(F, Result.CalleeDependencies, Callees);
    if (Result.DependencyDigest.empty())
      return {};
  }
  return Result;
}

std::string
lowSEHFrameDependencyDigest(const LowFunc &F, llvm::ArrayRef<va_t> Dependencies,
                            const std::map<va_t, const LowFunc *> *Callees) {
  const auto CallerDigest = lowSEHFrameDependencyDigest(F);
  if (Dependencies.empty())
    return CallerDigest;
  if (!Callees)
    return {};
  llvm::SHA256 Hash;
  Hash.update("neverd-low-seh-frame-callees-v1");
  Hash.update(CallerDigest);
  std::set<va_t> Seen;
  for (va_t Entry : Dependencies) {
    const auto It = Callees->find(Entry);
    if (Entry == F.Entry || !Seen.insert(Entry).second ||
        It == Callees->end() || !It->second || It->second->Entry != Entry)
      return {};
    Hash.update(lowSEHFrameDependencyDigest(*It->second));
  }
  return llvm::toHex(Hash.final(), true);
}

} // namespace neverd
