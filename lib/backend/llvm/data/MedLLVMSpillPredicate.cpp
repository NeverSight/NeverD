//===- MedLLVMSpillPredicate.cpp - Stack-spill predicates ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack-slot predicates for MedLLVMEmitter's global-data resolution:
/// whether a slot's address escapes, whether a matching-key load reloads
/// it, and whether such a reload is used locally.  Together they decide
/// whether a spilled global base keeps its original VA or is symbolized
/// at the store, plus the value-VA folding those walks rely on.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/object/SectionNames.h"

#define DEBUG_TYPE "neverd-med-llvm-global-data"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

void MedLLVMEmitter::ensureAddrPredCache() const {
  if (AddrPredCacheFor == CurMedFunc)
    return;
  AddrPredCacheFor = CurMedFunc;
  SlotAddressEscapesCache.clear();
  SlotMatchingKeyLoadCache.clear();
  SlotReloadUsedLocallyCache.clear();
  WritableDataSegCache.clear();
  PtrTableUniqueSegCache.clear();
}

bool MedLLVMEmitter::stackSlotAddressEscapes(const MedVar &SlotAddr) const {
  if (!CurMedFunc)
    return false;
  // Canonicalize the slot through register copies so a parameter-register copy
  // of the address compares equal to the load/store form (both ThroughRegs).
  auto Target = addrSlotKey(SlotAddr, /*Depth=*/0, /*ThroughRegs=*/true);
  if (!Target)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotAddressEscapesCache.find(*Target);
      It != SlotAddressEscapesCache.end())
    return It->second;
  bool Result = false;
  // The slot's address passed as a call argument: a callee may write an
  // already- symbolized pointer through it (the escaping output-pointer shape,
  // #475).
  for (const auto &CI : CurMedFunc->CallInfos) {
    for (const auto &Arg : CI.Args)
      if (auto K = addrSlotKey(Arg, 0, true); K && *K == *Target) {
        Result = true;
        break;
      }
    if (Result)
      break;
  }
  // The slot's address stored to memory escapes the same way.
  if (!Result)
    for (const auto &Blk : CurMedFunc->Blocks) {
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          if (auto K = addrSlotKey(Op.Inputs[1], 0, true); K && *K == *Target) {
            Result = true;
            break;
          }
      if (Result)
        break;
    }
  SlotAddressEscapesCache[*Target] = Result;
  return Result;
}

bool MedLLVMEmitter::frameSlotHasMatchingKeyLoad(
    const MedVar &StoreAddr) const {
  if (!CurMedFunc)
    return false;
  auto SK = addrSlotKey(StoreAddr);
  if (!SK)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotMatchingKeyLoadCache.find(*SK);
      It != SlotMatchingKeyLoadCache.end())
    return It->second;
  bool Result = false;
  for (const auto &Blk : CurMedFunc->Blocks) {
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
        if (auto LK = addrSlotKey(Op.Inputs[0]); LK && *LK == *SK) {
          Result = true;
          break;
        }
    if (Result)
      break;
  }
  SlotMatchingKeyLoadCache[*SK] = Result;
  return Result;
}

bool MedLLVMEmitter::collectFrameReloadSources(
    const MedOp &Load, std::vector<MedVar> &Sources) const {
  Sources.clear();
  if (!CurMedFunc || Load.Opcode != NdOp::LOAD || Load.NumInputs < 1 ||
      Load.Output.Size == 0 || !varIsFrameDerived(Load.Inputs[0]) ||
      stackSlotAddressEscapes(Load.Inputs[0]))
    return false;

  const auto Target = addrSlotKey(Load.Inputs[0]);
  if (!Target)
    return false;

  std::map<int, const MedBlock *> BlocksById;
  const MedBlock *LoadBlock = nullptr;
  size_t LoadIndex = 0;
  for (const MedBlock &Block : CurMedFunc->Blocks) {
    if (!BlocksById.emplace(Block.Id, &Block).second)
      return false;
    for (size_t I = 0; I < Block.Ops.size(); ++I)
      if (&Block.Ops[I] == &Load) {
        if (LoadBlock)
          return false;
        LoadBlock = &Block;
        LoadIndex = I;
      }
  }
  if (!LoadBlock)
    return false;

  // Preds is cached IR metadata, not authority for reachability.  Reconstruct
  // the incoming relation from every ordinary/exceptional successor and
  // require the two views to agree before proving an all-path reaching store.
  // A malformed or stale predecessor list must not hide an uninitialized
  // bypass and turn a later table-looking STORE into pointer provenance.
  std::map<int, std::set<int>> StructuralPreds;
  for (const auto &[Id, Block] : BlocksById) {
    (void)Block;
    StructuralPreds.emplace(Id, std::set<int>{});
  }
  for (const auto &[Id, Block] : BlocksById) {
    for (int SuccId : Block->Succs) {
      auto It = StructuralPreds.find(SuccId);
      if (It == StructuralPreds.end())
        return false;
      It->second.insert(Id);
    }
    for (const ExceptionalEdge &Edge : Block->ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue;
      auto It = StructuralPreds.find(Edge.BlockId);
      if (It == StructuralPreds.end())
        return false;
      It->second.insert(Id);
    }
  }
  for (const auto &[Id, Block] : BlocksById) {
    std::set<int> Declared(Block->Preds.begin(), Block->Preds.end());
    for (const ExceptionalEdge &Edge : Block->ExceptionalPreds)
      if (Edge.BlockId >= 0)
        Declared.insert(Edge.BlockId);
    if (Declared != StructuralPreds[Id])
      return false;
  }

  auto isMemoryWrite = [](NdOp Opcode) {
    return Opcode == NdOp::STORE || Opcode == NdOp::ATOMIC_XCHG ||
           Opcode == NdOp::ATOMIC_ADD || Opcode == NdOp::ATOMIC_CMPXCHG;
  };
  auto overlaps = [](int64_t A, uint16_t ASize, int64_t B, uint16_t BSize) {
    if (ASize == 0 || BSize == 0)
      return true;
    auto endsBefore = [](int64_t Start, uint16_t Size, int64_t Other) {
      return Start < Other &&
             static_cast<uint64_t>(Other) - static_cast<uint64_t>(Start) >=
                 Size;
    };
    return !endsBefore(A, ASize, B) && !endsBefore(B, BSize, A);
  };
  struct ReachingState {
    bool Reachable = false;
    bool Uninitialized = false;
    bool Invalid = false;
    std::vector<MedVar> Values;
  };
  auto addUnique = [](std::vector<MedVar> &Values, const MedVar &Value) {
    if (std::find(Values.begin(), Values.end(), Value) == Values.end())
      Values.push_back(Value);
  };
  auto sameState = [](const ReachingState &A, const ReachingState &B) {
    if (A.Reachable != B.Reachable || A.Uninitialized != B.Uninitialized ||
        A.Invalid != B.Invalid || A.Values.size() != B.Values.size())
      return false;
    for (const MedVar &Value : A.Values)
      if (std::find(B.Values.begin(), B.Values.end(), Value) == B.Values.end())
        return false;
    return true;
  };
  auto mergeInto = [&](ReachingState &Dst, const ReachingState &Src) {
    if (!Src.Reachable)
      return false;
    ReachingState Before = Dst;
    Dst.Reachable = true;
    Dst.Uninitialized |= Src.Uninitialized;
    Dst.Invalid |= Src.Invalid;
    for (const MedVar &Value : Src.Values)
      addUnique(Dst.Values, Value);
    return !sameState(Before, Dst);
  };
  auto transfer = [&](const MedBlock &Block, size_t Boundary,
                      ReachingState State) {
    if (!State.Reachable || Boundary > Block.Ops.size()) {
      State.Invalid |= Boundary > Block.Ops.size();
      return State;
    }
    for (size_t I = 0; I < Boundary; ++I) {
      const MedOp &Op = Block.Ops[I];
      if (!isMemoryWrite(Op.Opcode))
        continue;
      if (Op.NumInputs < 1) {
        State.Invalid = true;
        State.Values.clear();
        continue;
      }
      const MedVar &WriteAddr = Op.Inputs[0];
      if (!varIsFrameDerived(WriteAddr))
        continue;
      const auto WriteKey = addrSlotKey(WriteAddr);
      // A frame-derived write whose slot cannot be canonicalized, or whose
      // root differs from the reload's root, may still alias after an
      // unmodelled stack adjustment. Keep the state poisoned until a later
      // exact full-width STORE definitely overwrites the target slot.
      if (!WriteKey || WriteKey->first != Target->first) {
        State.Invalid = true;
        State.Values.clear();
        continue;
      }

      uint16_t WriteSize = Op.Opcode == NdOp::STORE && Op.NumInputs >= 2
                               ? Op.Inputs[1].Size
                               : Op.Output.Size;
      if (!overlaps(WriteKey->second, WriteSize, Target->second,
                    Load.Output.Size))
        continue;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 && WriteSize != 0 &&
          WriteKey->second == Target->second && WriteSize == Load.Output.Size) {
        State.Uninitialized = false;
        State.Invalid = false;
        State.Values.clear();
        addUnique(State.Values, Op.Inputs[1]);
        continue;
      }
      State.Uninitialized = false;
      State.Invalid = true;
      State.Values.clear();
    }
    return State;
  };

  // Forward may-reach dataflow over the exact slot.  Unlike a recursive
  // backwards walk, this reaches a fixed point across loop back-edges and
  // therefore distinguishes a preheader definition from a STORE that occurs
  // only after the LOAD and can affect later iterations.
  std::map<int, ReachingState> InStates;
  std::map<int, ReachingState> OutStates;
  ReachingState Entry;
  Entry.Reachable = true;
  Entry.Uninitialized = true;
  InStates[CurMedFunc->Blocks.front().Id] = Entry;
  std::vector<int> Work{CurMedFunc->Blocks.front().Id};
  while (!Work.empty()) {
    int BlockId = Work.back();
    Work.pop_back();
    auto BlockIt = BlocksById.find(BlockId);
    if (BlockIt == BlocksById.end())
      return false;
    const MedBlock &Block = *BlockIt->second;
    ReachingState Next = transfer(Block, Block.Ops.size(), InStates[BlockId]);
    if (sameState(OutStates[BlockId], Next))
      continue;
    OutStates[BlockId] = Next;

    auto propagate = [&](int SuccId) {
      auto Succ = BlocksById.find(SuccId);
      if (Succ == BlocksById.end())
        return false;
      if (mergeInto(InStates[SuccId], Next))
        Work.push_back(SuccId);
      return true;
    };
    for (int SuccId : Block.Succs)
      if (!propagate(SuccId))
        return false;
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      if (Edge.BlockId >= 0 && !propagate(Edge.BlockId))
        return false;
  }

  ReachingState AtLoad =
      transfer(*LoadBlock, LoadIndex, InStates[LoadBlock->Id]);
  if (!AtLoad.Reachable || AtLoad.Uninitialized || AtLoad.Invalid ||
      AtLoad.Values.empty())
    return false;
  Sources = std::move(AtLoad.Values);
  return true;
}

std::optional<uint64_t> MedLLVMEmitter::traceValueVA(const MedVar &V,
                                                     int Depth) const {
  if (!CurMedFunc && !V.isConst())
    return std::nullopt;
  (void)Depth;
  auto mask = [](uint64_t X, uint16_t Size) -> uint64_t {
    if (Size == 0 || Size >= 8)
      return X;
    return X & ((1ULL << (Size * 8)) - 1);
  };
  using Key = std::tuple<int, int, int, uint16_t>;
  std::set<Key> Active;
  std::map<Key, std::optional<uint64_t>> Memo;
  std::function<std::optional<uint64_t>(const MedVar &)> Eval =
      [&](const MedVar &Cur) -> std::optional<uint64_t> {
    if (Cur.isConst())
      return Cur.ConstVal;
    Key K = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                            Cur.Size);
    if (auto It = Memo.find(K); It != Memo.end())
      return It->second;
    if (!Active.insert(K).second)
      return std::nullopt;

    std::optional<uint64_t> Result;
    const MedOp *Def = lookupDef(Cur);
    if (Def) {
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        if (Def->NumInputs >= 1)
          if (auto B = Eval(Def->Inputs[0]))
            Result = mask(mask(*B, Def->Inputs[0].Size), Def->Output.Size);
        break;
      case NdOp::INT_SEXT:
        if (Def->NumInputs >= 1)
          if (auto B = Eval(Def->Inputs[0])) {
            unsigned InBits = Def->Inputs[0].Size * 8;
            uint64_t Value = mask(*B, Def->Inputs[0].Size);
            if (InBits != 0 && InBits < 64 &&
                (Value & (uint64_t(1) << (InBits - 1))))
              Value |= ~((uint64_t(1) << InBits) - 1);
            Result = mask(Value, Def->Output.Size);
          }
        break;
      case NdOp::SUBBYTES:
        if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
            Def->Inputs[1].ConstVal == 0)
          if (auto B = Eval(Def->Inputs[0]))
            Result = mask(*B, Def->Output.Size);
        break;
      case NdOp::INT_ADD:
        if (Def->NumInputs >= 2)
          if (auto A = Eval(Def->Inputs[0]))
            if (auto B = Eval(Def->Inputs[1]))
              Result = mask(*A + *B, Def->Output.Size);
        break;
      case NdOp::INT_SUB:
        if (Def->NumInputs >= 2)
          if (auto A = Eval(Def->Inputs[0]))
            if (auto B = Eval(Def->Inputs[1]))
              Result = mask(*A - *B, Def->Output.Size);
        break;
      default:
        break;
      }
    }
    Active.erase(K);
    Memo.emplace(K, Result);
    return Result;
  };
  return Eval(V);
}

bool MedLLVMEmitter::frameSlotReloadUsedLocally(const MedVar &StoreAddr) const {
  if (!CurMedFunc)
    return false;
  auto SK = addrSlotKey(StoreAddr);
  if (!SK)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotReloadUsedLocallyCache.find(*SK);
      It != SlotReloadUsedLocallyCache.end())
    return It->second;

  auto isFwd = [](NdOp Op) {
    return Op == NdOp::COPY || Op == NdOp::INT_ZEXT || Op == NdOp::INT_SEXT ||
           Op == NdOp::SUBBYTES;
  };
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  auto compute = [&]() -> bool {
    // The reload values (LOAD outputs of slot SK) plus everything they flow
    // into through pure-forwarding ops (COPY/widen/low-slice) — the values that
    // still carry the reloaded pointer.
    std::vector<MedVar> ReloadVals;
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
          if (auto LK = addrSlotKey(Op.Inputs[0]); LK && *LK == *SK)
            ReloadVals.push_back(Op.Output);
    if (ReloadVals.empty())
      return false;
    auto inReloadSet = [&](const MedVar &V) {
      for (const auto &R : ReloadVals)
        if (sameVar(V, R))
          return true;
      return false;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const auto &Blk : CurMedFunc->Blocks)
        for (const auto &Op : Blk.Ops)
          if (isFwd(Op.Opcode) && Op.NumInputs >= 1 &&
              inReloadSet(Op.Inputs[0]) && !inReloadSet(Op.Output)) {
            if (Op.Opcode == NdOp::SUBBYTES &&
                !(Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                  Op.Inputs[1].ConstVal == 0))
              continue; // a high-slice extract drops the pointer, not
                        // forwarding
            ReloadVals.push_back(Op.Output);
            Changed = true;
          }
    }
    // Any NON-forwarding op consuming a reloaded value uses the pointer locally
    // (dereference address, `p++`, `p - base`, comparison) — so it must keep
    // the original VA.  Pure forwarding to the return register, and a RETURN
    // that takes the reload directly as its value operand (x86-64 lowers
    // `return p` to `RETURN <reload>`), are escapes, not local uses.
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if (isFwd(Op.Opcode) || Op.Opcode == NdOp::RETURN)
          continue;
        for (int I = 0; I < Op.NumInputs; ++I)
          if (inReloadSet(Op.Inputs[I]))
            return true;
      }
    return false;
  };

  bool Result = compute();
  SlotReloadUsedLocallyCache[*SK] = Result;
  return Result;
}

} // namespace neverd
