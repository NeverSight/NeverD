//===- ResolverFiniteState.h - Bounded scalar CFG fixed point -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact known-byte states for finite lookup loops. Unknown values are never
/// initialized to zero. Unknown conditions explore both edges; a fixed point
/// must close before any accumulated selector envelope becomes evidence.

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERFINITESTATE_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERFINITESTATE_H

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::detail {

// The graph is the resolver's immutable, rooted snapshot. Keeping this helper
// private avoids introducing a second CFG or a persistent per-image state.
template <typename GraphT, typename ConditionEdges, typename Consume>
std::optional<uint64_t> readonlyScalarEnvelope(
    const GraphT &Graph, const BinaryImage &Image, const NdVar &Candidate,
    std::pair<int, int> Point, uint64_t Bound, ConditionEdges Edges,
    Consume ConsumeWork, size_t *SharedWork, bool &Complete) {
  Complete = true;
  if (!Bound || Bound > 64 || !Candidate.Size || Candidate.Size > 8 ||
      !supportsImmutableImageReads(Image) || !Graph.InstructionGuards.empty())
    return {};
  size_t Remaining = limits::kMaxJumpTableScalarStateWork;
  auto charge = [&](size_t N) {
    if (!Complete || N > Remaining || !ConsumeWork(N)) {
      Complete = false;
      return false;
    }
    Remaining -= N;
    return true;
  };
  if (!charge(Graph.Blocks.size() * 3 + Graph.RootBlocks.size()))
    return {};
  std::vector<bool> Reaches(Graph.Blocks.size(), false);
  std::vector<int> Pending{Point.first};
  Reaches[Point.first] = true;
  for (size_t I = 0; I < Pending.size(); ++I) {
    const auto &Block = Graph.Blocks[Pending[I]];
    if (!charge(Block.Preds.size() * 2))
      return {};
    for (int Pred : Block.Preds)
      if (!Reaches[Pred]) {
        Reaches[Pred] = true;
        Pending.push_back(Pred);
      }
  }
  // Stopping at the observation is sound only if no later edge can revisit
  // it. Recurrent dispatches retain the existing joined-domain owner.
  for (int Succ : Graph.Blocks[Point.first].Succs)
    if (Reaches[Succ])
      return {};

  // Keep address origin with every byte so a partial write cannot retain
  // the pointer authority of an older full-width definition.
  using Byte = std::pair<uint8_t, bool>;
  using Bytes = std::map<uint64_t, Byte>;
  using State = std::pair<int, Bytes>;
  std::set<State> Seen;
  std::deque<const State *> Work;
  auto lookupCost = [](size_t N) { return 1 + std::bit_width(N); };
  auto enqueue = [&](int Block, const Bytes &Registers) {
    if (!Reaches[Block])
      return true;
    // Account for lexicographic comparisons, copying, and retained lifetime
    // before allocating a state. The worklist borrows stable set nodes.
    if (!charge((Registers.size() * 2 + 1) * (lookupCost(Seen.size()) + 3)))
      return false;
    const auto [It, Inserted] = Seen.emplace(Block, Registers);
    if (Inserted)
      Work.push_back(&*It);
    return true;
  };
  for (int Root : Graph.RootBlocks)
    if (!enqueue(Root, {}))
      return {};
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto Preserved = TRI.callPreservedRanges(Image.abiFormat());
  std::map<std::pair<va_t, uint16_t>, std::optional<uint64_t>> ReadCache;
  bool ReadImmutable = false;
  uint64_t Mask = 0;
  while (!Work.empty()) {
    const auto &[BlockIndex, Incoming] = *Work.front();
    Work.pop_front();
    if (!charge(Incoming.size() * 3 + 1))
      return {};
    Bytes Registers = Incoming, Temps;
    const auto &Block = Graph.Blocks[BlockIndex];
    if (Block.HasOpaqueTerminator || Block.HasResumableTerminator)
      return {};
    if (!charge(Block.Ops.size() +
                (Block.LastInsn ? Block.LastInsn->Ops.size() : 0)))
      return {};
    const auto Branch = Block.LastInsn ? Edges(*Block.LastInsn) : std::nullopt;
    std::optional<bool> Condition;
    va_t Instruction = InvalidVA;
    auto valueOf = [&](const NdVar &V) -> std::optional<uint64_t> {
      if (!V.Size || V.Size > 8)
        return {};
      if (V.isConst())
        return V.Offset & (UINT64_MAX >> ((8 - V.Size) * 8));
      if ((!V.isReg() && !V.isTemp()) || V.Offset > UINT64_MAX - V.Size)
        return {};
      const auto &Storage = V.isReg() ? Registers : Temps;
      if (!charge(V.Size * lookupCost(Storage.size())))
        return {};
      uint64_t Value = 0;
      for (unsigned I = 0; I < V.Size; ++I) {
        const auto Found = Storage.find(V.Offset + I);
        if (Found == Storage.end())
          return {};
        Value |= uint64_t{Found->second.first} << (I * 8);
      }
      return Value;
    };
    auto isAddress = [&](const NdVar &V) {
      if (V.Size != Image.getPointerSize())
        return false;
      if (V.isConst())
        return isExactAddressProvenance(V.Provenance);
      if (!V.isReg() && !V.isTemp())
        return false;
      const auto &Storage = V.isReg() ? Registers : Temps;
      if (!charge(V.Size * lookupCost(Storage.size())))
        return false;
      for (unsigned I = 0; I < V.Size; ++I) {
        const auto Found = Storage.find(V.Offset + I);
        if (Found == Storage.end() || !Found->second.second)
          return false;
      }
      return true;
    };
    bool Observed = false;
    for (int I = 0; I < static_cast<int>(Block.Ops.size()); ++I) {
      const LowOp &Op = Block.Ops[I];
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (BlockIndex == Point.first && I == Point.second) {
        const auto Value = valueOf(Candidate);
        if (!Value || *Value >= Bound)
          return {};
        Mask |= uint64_t{1} << *Value;
        Observed = true;
        break;
      }
      if (Branch && Op.Addr == Branch->Condition->Addr &&
          Op.Seq == Branch->Condition->Seq) {
        if (auto Value = valueOf(Op.Inputs[1]))
          Condition = *Value != 0;
        continue;
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (!charge(Registers.size() * (Preserved.size() + 1)))
          return {};
        std::erase_if(Registers, [&](const auto &Byte) {
          return std::none_of(Preserved.begin(), Preserved.end(),
                              [&](const TargetRegisterRange &Range) {
                                return Byte.first >= Range.Offset &&
                                       Byte.first - Range.Offset < Range.Bytes;
                              });
        });
        Temps.clear();
      } else if (Op.Opcode == NdOp::INTRINSIC &&
                 !lowIntrinsicHasOnlyValueEffects(Op) &&
                 !lowIntrinsicWritesOnlyItsOutput(Op)) {
        // An unmodeled intrinsic may change implicit registers. Clearing
        // facts permits later independent constants without guessing effects.
        Registers.clear();
        Temps.clear();
      }
      auto Value = lowScalarConstantResult(Op, valueOf);
      bool AddressValue = false;
      if (Value && Op.Output.Size == Image.getPointerSize()) {
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
          AddressValue = isAddress(Op.Inputs[0]);
        else if (Op.NumInputs == 2 &&
                 (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB)) {
          const bool Left = isAddress(Op.Inputs[0]);
          const bool Right = isAddress(Op.Inputs[1]);
          AddressValue =
              Op.Opcode == NdOp::INT_ADD ? Left != Right : Left && !Right;
        }
      }
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 && Op.Output.Size &&
          Op.Output.Size <= 8 && Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          isAddress(Op.Inputs[0])) {
        // The image's own headers describe the image that runs, not this
        // file: a read of them stays unknown (BinaryImage::imageBaseOffset).
        if (auto Address = valueOf(Op.Inputs[0]);
            Address && !Image.imageBaseOffset(*Address)) {
          if (!charge(lookupCost(ReadCache.size()) * 2 + 8))
            return {};
          auto [It, Inserted] =
              ReadCache.try_emplace({*Address, Op.Output.Size});
          if (Inserted) {
            // The loader reserves its complete metadata traversal before
            // reading. Its allowance must fit both enclosing accounts.
            const size_t ReadAllowance =
                SharedWork ? std::min(Remaining, *SharedWork) : Remaining;
            size_t ReadWork = ReadAllowance;
            It->second = readImmutableImageScalar(Image, *Address,
                                                  Op.Output.Size, &ReadWork);
            if (ReadWork == ReadAllowance) {
              Complete = false;
              return {};
            }
            if (!charge(ReadAllowance - ReadWork))
              return {};
          }
          Value = It->second;
          ReadImmutable |= Value.has_value();
        }
      }
      if (Op.Output.isReg() || Op.Output.isTemp()) {
        auto &Storage = Op.Output.isReg() ? Registers : Temps;
        const auto &Out = Op.Output;
        if (Out.Offset > UINT64_MAX - Out.Size ||
            !charge(Storage.size() + 16 * lookupCost(Storage.size()) + 32))
          return {};
        // Wide unknown writes invalidate existing bytes without allocating
        // one entry per byte of an attacker-controlled vector width.
        std::erase_if(Storage, [&](const auto &Byte) {
          return Byte.first >= Out.Offset && Byte.first - Out.Offset < Out.Size;
        });
        if (Value)
          for (unsigned J = 0; J < Out.Size; ++J)
            Storage[Out.Offset + J] = {uint8_t(*Value >> (8 * J)),
                                       AddressValue};
        if (Out.isReg() && Out.Size < 8 &&
            TRI.writeZeroExtends(Out.Offset, Out.Size))
          for (unsigned J = Out.Size; J < 8; ++J)
            Storage[Out.Offset + J] = {0, false};
      }
      if (!Complete)
        return {};
    }
    if (!Observed) {
      if (!charge(Block.Succs.size()))
        return {};
      for (int Succ : Block.Succs) {
        if (Branch && Condition &&
            Graph.Blocks[Succ].Start !=
                (*Condition ? Branch->TrueTarget : Branch->FalseTarget))
          continue;
        if (!enqueue(Succ, Registers))
          return {};
      }
    }
  }
  return Complete && ReadImmutable && Mask ? std::optional<uint64_t>{Mask}
                                           : std::nullopt;
}

} // namespace neverd::detail

#endif
