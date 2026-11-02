//===- CFGBuilderX86BoundedLeaf.cpp - Proven SGX and SMX selectors -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Meet exact byte facts over every predecessor, distinguishing an unreachable
/// edge from an unknown value. Rewrite only the normalized graph, so discovery
/// of another edge can revoke a proof without retaining a guessed leaf.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/lift/X86/X86BoundedLeafState.h"

#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace neverd {
namespace {
Intrinsic pendingLeaf(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs || !Op.Inputs[0].isConst())
    return Intrinsic::None;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  return isX86BoundedLeafPending(Id) ? Id : Intrinsic::None;
}
using RegisterFacts = std::map<uint64_t, uint8_t>;
} // namespace

void CFGBuilder::resolveX86BoundedLeaves(LowFunc &Func) {
  if (!CurrentImg ||
      (CurrentImg->Arch != Arch::X86 && CurrentImg->Arch != Arch::X64))
    return;
  bool HasPending = false;
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops)
      HasPending |= pendingLeaf(Op) != Intrinsic::None;
  if (!HasPending)
    return;
  if (auto Error = validateLowInstructionBoundaries(
          Func, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return;
  }
  bool Complete = true;
  auto Spend = [&](size_t Work) {
    if (Work > RemainingX86BoundedLeafWork) {
      RemainingX86BoundedLeafWork = 0;
      Complete = false;
      return false;
    }
    RemainingX86BoundedLeafWork -= Work;
    return true;
  };
  if (!Spend(Func.Blocks.size()))
    return;
  std::map<int, size_t> Indices;
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    if (!Indices.emplace(Func.Blocks[I].Id, I).second)
      return;
  std::vector<std::optional<RegisterFacts>> Outputs(Func.Blocks.size());
  std::vector<std::map<va_t, uint64_t>> Selectors(Func.Blocks.size());
  std::deque<size_t> Worklist;
  std::vector<bool> Queued(Func.Blocks.size(), true);
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    Worklist.push_back(I);
  auto Independent = [&](va_t Address) {
    return Address == Func.Entry || Func.ModuleAnalysisRoots.count(Address) ||
           CurrentImg->CodeRefTargets.count(Address);
  };
  while (!Worklist.empty() && Complete) {
    const auto Index = Worklist.front();
    Worklist.pop_front();
    Queued[Index] = false;
    const auto &Block = Func.Blocks[Index];
    if (!Spend(1 + Block.Preds.size() + Block.Ops.size()))
      break;
    std::optional<RegisterFacts> Incoming;
    if (Block.Preds.empty() || !Block.ExceptionalPreds.empty() ||
        Independent(Block.StartAddr))
      Incoming.emplace();
    for (int Pred : Block.Preds) {
      const auto At = Indices.find(Pred);
      if (At == Indices.end())
        return;
      const auto &Facts = Outputs[At->second];
      if (!Facts)
        continue;
      if (!Incoming) {
        if (!Spend(Facts->size()))
          break;
        Incoming = Facts;
      } else {
        if (!Spend(Incoming->size()))
          break;
        for (auto It = Incoming->begin(); It != Incoming->end();) {
          const auto Value = Facts->find(It->first);
          It = Value == Facts->end() || Value->second != It->second
                   ? Incoming->erase(It)
                   : std::next(It);
        }
      }
    }
    if (!Incoming || !Complete)
      continue;
    auto Registers = std::move(*Incoming);
    std::map<uint64_t, std::pair<uint16_t, uint64_t>> Temps;
    Selectors[Index].clear();
    auto ValueOf = [&](const NdVar &V) -> std::optional<uint64_t> {
      if (!Spend(1 + V.Size) || !V.Size || V.Size > sizeof(uint64_t))
        return std::nullopt;
      const uint64_t Mask = UINT64_MAX >> ((sizeof(uint64_t) - V.Size) * 8);
      if (V.isConst()) {
        if (isAddressProvenance(V.Provenance) ||
            (V.Provenance == ConstantAddressProvenance::Unknown &&
             CurrentImg->isPotentiallyRelocatableAddress(V.Offset)))
          return std::nullopt;
        return V.Offset & Mask;
      }
      if (V.isTemp()) {
        const auto It = Temps.find(V.Offset);
        return It != Temps.end() && It->second.first == V.Size
                   ? std::optional<uint64_t>{It->second.second}
                   : std::nullopt;
      }
      if (!V.isReg() || V.Offset > UINT64_MAX - V.Size)
        return std::nullopt;
      uint64_t Result = 0;
      for (unsigned I = 0; I < V.Size; ++I) {
        const auto It = Registers.find(V.Offset + I);
        if (It == Registers.end())
          return std::nullopt;
        Result |= uint64_t{It->second} << (I * 8);
      }
      return Result;
    };
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (!Spend(1 + Temps.size()))
        break;
      Temps.clear();
      if (Independent(Boundary.Address)) {
        if (!Spend(Registers.size()))
          break;
        Registers.clear();
      }
      const auto Raw = Insns.find(Boundary.Address);
      if (Raw == Insns.end())
        return;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I) {
        const auto &Op = Block.Ops[I];
        auto Result = lowScalarConstantResult(Op, ValueOf);
        const auto Pending = pendingLeaf(Op);
        if (Pending != Intrinsic::None) {
          if (!x86BoundedLeafShapeIsValid(
                  Pending, x86BoundedLeafLowShape(Op, CurrentImg->Arch)))
            return;
          const auto Ax = ValueOf(Op.Inputs[1]);
          const auto C =
              Ax ? x86BoundedLeafContract(Pending, *Ax) : std::nullopt;
          if (C) {
            Selectors[Index][Boundary.Address] = *Ax;
            // A preserving leaf can feed a later leaf without inventing a
            // write. Seed its raw auxiliary placeholders for this proof only.
            if (!(C->Writes & 1))
              Result = Ax;
            for (unsigned K = 1; K <= X86BoundedLeafAuxiliaryOutputs; ++K) {
              if (I + K >= Boundary.FirstOp + Boundary.OpCount)
                return;
              const auto &Copy = Block.Ops[I + K];
              if (Copy.Opcode != NdOp::COPY || Copy.NumInputs != 1 ||
                  !Copy.Inputs[0].isTemp())
                return;
              if (K < X86BoundedLeafRegisterCount
                      ? !(C->Writes & (1u << K))
                      : C->Flags == X86BoundedLeafFlags::Preserve) {
                const auto Value = ValueOf(Op.Inputs[K + 1]);
                if (Value)
                  Temps[Copy.Inputs[0].Offset] = {Copy.Inputs[0].Size, *Value};
              }
            }
          } else {
            if (!Spend(Registers.size() + Temps.size()))
              break;
            Registers.clear();
            Temps.clear();
          }
        } else if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
                   Op.Opcode == NdOp::INTRINSIC) {
          if (!Spend(Registers.size() + Temps.size()))
            break;
          Registers.clear();
          Temps.clear();
        }
        const auto &V = Op.Output;
        if (!Spend(1 + V.Size))
          break;
        if (V.isTemp()) {
          if (Result)
            Temps[V.Offset] = {V.Size, *Result};
          else
            Temps.erase(V.Offset);
        } else if (V.isReg()) {
          if (V.Offset > UINT64_MAX - V.Size)
            return;
          for (unsigned K = 0; K < V.Size; ++K)
            if (Result)
              Registers[V.Offset + K] = uint8_t(*Result >> (K * 8));
            else
              Registers.erase(V.Offset + K);
        }
      }
      if (Raw->second.IsOpaqueTerminator || Raw->second.IsInstructionGuard) {
        if (!Spend(Registers.size() + Temps.size()))
          break;
        Registers.clear();
        Temps.clear();
      }
    }
    if (!Complete)
      break;
    if (!Spend(Registers.size() +
               (Outputs[Index] ? Outputs[Index]->size() : 0)))
      break;
    if (Outputs[Index] && *Outputs[Index] == Registers)
      continue;
    Outputs[Index] = std::move(Registers);
    for (int Succ : Block.Succs) {
      if (!Spend(1))
        break;
      const auto At = Indices.find(Succ);
      if (At == Indices.end())
        return;
      if (!Queued[At->second]) {
        Queued[At->second] = true;
        Worklist.push_back(At->second);
      }
    }
  }
  if (!Complete)
    return;
  // No result is published from an incomplete fixed point.
  for (size_t BI = 0; BI < Func.Blocks.size(); ++BI) {
    auto &Block = Func.Blocks[BI];
    std::vector<LowOp> Ops;
    for (auto &Boundary : Block.InstructionBoundaries) {
      const size_t Begin = Ops.size();
      const auto At = Selectors[BI].find(Boundary.Address);
      if (At == Selectors[BI].end())
        Ops.insert(Ops.end(), Block.Ops.begin() + Boundary.FirstOp,
                   Block.Ops.begin() + Boundary.FirstOp + Boundary.OpCount);
      else {
        Intrinsic Id = Intrinsic::None;
        for (size_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount; ++I)
          if (const auto P = pendingLeaf(Block.Ops[I]); P != Intrinsic::None)
            Id = x86BoundedLeafResolvedId(P);
        LiftStateBase State(Boundary.Address, Boundary.Size, Ops);
        liftBoundedLeafState(State, CurrentImg->Arch, Id, At->second);
      }
      Boundary.FirstOp = Begin;
      Boundary.OpCount = Ops.size() - Begin;
    }
    Block.Ops = std::move(Ops);
  }
}

} // namespace neverd
