//===- StackSlotFlow.h - Reaching stack-slot values ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_STACKSLOTFLOW_H
#define NEVERD_LIB_SAFETY_STACKSLOTFLOW_H

#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::safety::detail {

struct ReachingStackValues {
  bool Complete = false;
  std::vector<MedVar> Values;
};

inline bool isStackMemoryWrite(NdOp Opcode) {
  return Opcode == NdOp::STORE || Opcode == NdOp::ATOMIC_XCHG ||
         Opcode == NdOp::ATOMIC_ADD || Opcode == NdOp::ATOMIC_CMPXCHG;
}

inline const MedVar *memoryAddress(const MedOp &Op) {
  if (Op.NumInputs == 0)
    return nullptr;
  if (Op.Opcode == NdOp::LOAD)
    return &Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
  if (Op.Opcode == NdOp::STORE)
    return &Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
  return &Op.Inputs[0];
}

inline const MedVar *storedValue(const MedOp &Op) {
  if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
    return nullptr;
  return &Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
}

inline bool stackRangesOverlap(int64_t A, uint16_t ASize, int64_t B,
                               uint16_t BSize) {
  if (ASize == 0 || BSize == 0)
    return true;
  auto endsBefore = [](int64_t Start, uint16_t Size, int64_t Other) {
    return Start < Other &&
           static_cast<uint64_t>(Other) - static_cast<uint64_t>(Start) >= Size;
  };
  return !endsBefore(A, ASize, B) && !endsBefore(B, BSize, A);
}

inline std::optional<int64_t> signedStackConstant(const MedVar &Value) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t))
    return std::nullopt;
  const unsigned Bits = static_cast<unsigned>(Value.Size) * 8;
  const uint64_t Mask = Bits == 64 ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t{1} << Bits) - 1;
  const uint64_t Truncated = Value.ConstVal & Mask;
  const uint64_t Sign = uint64_t{1} << (Bits - 1);
  if ((Truncated & Sign) == 0)
    return static_cast<int64_t>(Truncated);
  return -1 - static_cast<int64_t>((~Truncated) & Mask);
}

inline std::optional<int64_t> checkedStackOffset(int64_t Base, int64_t Delta,
                                                 bool Subtract) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  if (!Subtract) {
    if ((Delta > 0 && Base > Max - Delta) || (Delta < 0 && Base < Min - Delta))
      return std::nullopt;
    return Base + Delta;
  }
  if ((Delta > 0 && Base < Min + Delta) || (Delta < 0 && Base > Max + Delta))
    return std::nullopt;
  return Base - Delta;
}

template <typename ResolveOffset, typename MayBeFrameAddress>
ReachingStackValues
reachingStackValues(const MedFunc &F, int LoadBlockId, int LoadOpIdx,
                    int64_t TargetOffset, uint16_t LoadSize,
                    ResolveOffset Resolve, MayBeFrameAddress MayBeFrame) {
  ReachingStackValues Result;
  if (LoadOpIdx < 0 || LoadSize == 0)
    return Result;

  std::map<int, const MedBlock *> Blocks;
  for (const MedBlock &Block : F.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second)
      return Result;
  auto LoadIt = Blocks.find(LoadBlockId);
  if (LoadIt == Blocks.end() ||
      LoadOpIdx >= static_cast<int>(LoadIt->second->Ops.size()) ||
      LoadIt->second->Ops[LoadOpIdx].Opcode != NdOp::LOAD)
    return Result;

  std::map<int, std::set<int>> Preds;
  for (const auto &[Id, Block] : Blocks) {
    (void)Block;
    Preds.emplace(Id, std::set<int>{});
  }
  for (const auto &[Id, Block] : Blocks) {
    for (int Succ : Block->Succs) {
      auto It = Preds.find(Succ);
      if (It == Preds.end())
        return Result;
      It->second.insert(Id);
    }
    for (const ExceptionalEdge &Edge : Block->ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue;
      auto It = Preds.find(Edge.BlockId);
      if (It == Preds.end())
        return Result;
      It->second.insert(Id);
    }
  }
  for (const auto &[Id, Block] : Blocks) {
    std::set<int> Declared(Block->Preds.begin(), Block->Preds.end());
    for (const ExceptionalEdge &Edge : Block->ExceptionalPreds)
      if (Edge.BlockId >= 0)
        Declared.insert(Edge.BlockId);
    if (Declared != Preds[Id])
      return Result;
  }

  const MedBlock *Entry = nullptr;
  for (const auto &[Id, Block] : Blocks)
    if (Block->StartAddr == F.Entry) {
      if (Entry)
        return Result;
      Entry = Block;
    }
  if (!Entry)
    for (const auto &[Id, Block] : Blocks)
      if (Preds[Id].empty()) {
        if (Entry)
          return Result;
        Entry = Block;
      }
  if (!Entry)
    return Result;

  struct State {
    bool Reachable = false;
    bool Uninitialized = false;
    bool Invalid = false;
    std::vector<MedVar> Values;
  };
  auto addUnique = [](std::vector<MedVar> &Values, const MedVar &Value) {
    if (std::find(Values.begin(), Values.end(), Value) == Values.end())
      Values.push_back(Value);
  };
  auto same = [](const State &A, const State &B) {
    if (A.Reachable != B.Reachable || A.Uninitialized != B.Uninitialized ||
        A.Invalid != B.Invalid || A.Values.size() != B.Values.size())
      return false;
    for (const MedVar &Value : A.Values)
      if (std::find(B.Values.begin(), B.Values.end(), Value) == B.Values.end())
        return false;
    return true;
  };
  auto merge = [&](State &Dst, const State &Src) {
    if (!Src.Reachable)
      return false;
    State Before = Dst;
    Dst.Reachable = true;
    Dst.Uninitialized |= Src.Uninitialized;
    Dst.Invalid |= Src.Invalid;
    for (const MedVar &Value : Src.Values)
      addUnique(Dst.Values, Value);
    return !same(Before, Dst);
  };
  auto invalidate = [](State &S) {
    S.Uninitialized = false;
    S.Invalid = true;
    S.Values.clear();
  };
  auto transfer = [&](const MedBlock &Block, size_t Boundary, State S) {
    if (!S.Reachable || Boundary > Block.Ops.size()) {
      S.Invalid |= Boundary > Block.Ops.size();
      return S;
    }
    for (size_t I = 0; I < Boundary; ++I) {
      const MedOp &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (const MedCallInfo *CI = F.findCall(Block.Id, static_cast<int>(I)))
          for (const MedVar &Arg : CI->Args)
            if (auto Off = Resolve(Arg); Off && *Off == TargetOffset) {
              invalidate(S);
              break;
            }
        continue;
      }
      if (!isStackMemoryWrite(Op.Opcode))
        continue;
      const MedVar *Addr = memoryAddress(Op);
      if (!Addr) {
        invalidate(S);
        continue;
      }
      std::optional<int64_t> WriteOffset = Resolve(*Addr);
      if (!WriteOffset) {
        if (MayBeFrame(*Addr))
          invalidate(S);
        continue;
      }
      const MedVar *Value = storedValue(Op);
      const uint16_t WriteSize = Value ? Value->Size : Op.Output.Size;
      if (!stackRangesOverlap(*WriteOffset, WriteSize, TargetOffset,
                              LoadSize)) {
        if (Value)
          if (auto Escaped = Resolve(*Value);
              Escaped && *Escaped == TargetOffset)
            invalidate(S);
        continue;
      }
      if (Value && WriteSize == LoadSize && *WriteOffset == TargetOffset) {
        S.Uninitialized = false;
        S.Invalid = false;
        S.Values.clear();
        addUnique(S.Values, *Value);
      } else {
        invalidate(S);
      }
    }
    return S;
  };

  std::map<int, State> In;
  std::map<int, State> Out;
  State EntryState;
  EntryState.Reachable = true;
  EntryState.Uninitialized = true;
  In[Entry->Id] = EntryState;
  std::vector<int> Work{Entry->Id};
  while (!Work.empty()) {
    int Id = Work.back();
    Work.pop_back();
    auto It = Blocks.find(Id);
    if (It == Blocks.end())
      return Result;
    State Next = transfer(*It->second, It->second->Ops.size(), In[Id]);
    if (same(Out[Id], Next))
      continue;
    Out[Id] = Next;
    auto propagate = [&](int Succ) {
      if (!Blocks.count(Succ))
        return false;
      if (merge(In[Succ], Next))
        Work.push_back(Succ);
      return true;
    };
    for (int Succ : It->second->Succs)
      if (!propagate(Succ))
        return Result;
    for (const ExceptionalEdge &Edge : It->second->ExceptionalSuccs)
      if (Edge.BlockId >= 0 && !propagate(Edge.BlockId))
        return Result;
  }

  State AtLoad = transfer(*LoadIt->second, static_cast<size_t>(LoadOpIdx),
                          In[LoadBlockId]);
  if (!AtLoad.Reachable || AtLoad.Uninitialized || AtLoad.Invalid ||
      AtLoad.Values.empty())
    return Result;
  Result.Complete = true;
  Result.Values = std::move(AtLoad.Values);
  return Result;
}

} // namespace neverd::safety::detail

#endif // NEVERD_LIB_SAFETY_STACKSLOTFLOW_H
