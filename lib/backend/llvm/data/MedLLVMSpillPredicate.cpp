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
      Load.Output.Size == 0 ||
      Load.Output.Size != getTargetRegInfo(TargetArch).PointerSize ||
      !varIsFrameDerived(Load.Inputs[0]) ||
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
  auto addSource = [&](const MedVar &Source) {
    if (std::find(Sources.begin(), Sources.end(), Source) == Sources.end())
      Sources.push_back(Source);
  };

  std::set<std::pair<int, size_t>> Done;
  std::set<int> ActiveBlocks;
  std::function<bool(const MedBlock &, size_t)> Walk =
      [&](const MedBlock &Block, size_t Boundary) -> bool {
    if (Boundary > Block.Ops.size())
      return false;
    const auto State = std::make_pair(Block.Id, Boundary);
    if (Done.count(State))
      return true;
    // Re-entering a block before a reaching definition proves only a loop
    // back-edge, not that the first trip to the reload was initialized.  In
    // particular, do not let a STORE after the LOAD become its own reaching
    // definition through a self-edge.
    if (!ActiveBlocks.insert(Block.Id).second)
      return false;

    bool Result = [&]() {
      for (size_t I = Boundary; I > 0; --I) {
        const MedOp &Op = Block.Ops[I - 1];
        if (!isMemoryWrite(Op.Opcode))
          continue;
        if (Op.NumInputs < 1)
          return false;
        const MedVar &WriteAddr = Op.Inputs[0];
        if (!varIsFrameDerived(WriteAddr))
          continue;
        const auto WriteKey = addrSlotKey(WriteAddr);
        // A frame-derived write whose slot cannot be canonicalized, or whose
        // root differs from the reload's root, may still alias after an
        // unmodelled stack adjustment.  It cannot participate in a proof.
        if (!WriteKey || WriteKey->first != Target->first)
          return false;

        uint16_t WriteSize = Op.Opcode == NdOp::STORE && Op.NumInputs >= 2
                                 ? Op.Inputs[1].Size
                                 : Op.Output.Size;
        if (!overlaps(WriteKey->second, WriteSize, Target->second,
                      Load.Output.Size))
          continue;
        if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 || WriteSize == 0 ||
            WriteKey->second != Target->second || WriteSize != Load.Output.Size)
          return false;
        addSource(Op.Inputs[1]);
        return true;
      }

      std::set<int> PredIds(Block.Preds.begin(), Block.Preds.end());
      for (const ExceptionalEdge &Edge : Block.ExceptionalPreds)
        PredIds.insert(Edge.BlockId);
      if (PredIds.empty())
        return false;
      for (int PredId : PredIds) {
        auto It = BlocksById.find(PredId);
        if (PredId < 0 || It == BlocksById.end() ||
            !Walk(*It->second, It->second->Ops.size()))
          return false;
      }
      return true;
    }();

    ActiveBlocks.erase(Block.Id);
    if (Result)
      Done.insert(State);
    return Result;
  };

  return Walk(*LoadBlock, LoadIndex) && !Sources.empty();
}

std::optional<uint64_t> MedLLVMEmitter::traceValueVA(const MedVar &V,
                                                     int Depth) const {
  if (V.isConst())
    return V.ConstVal;
  if (!CurMedFunc || Depth > 12)
    return std::nullopt;
  const MedOp *Def = lookupDef(V);
  if (!Def)
    return std::nullopt;
  auto mask = [](uint64_t X, uint16_t Size) -> uint64_t {
    if (Size == 0 || Size >= 8)
      return X;
    return X & ((1ULL << (Size * 8)) - 1);
  };
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    // Width change preserves the low value an address carries; no mask needed
    // (a zero-extended narrow value already fits, a sign-extended address stays
    // positive in the low bits the rebase consumes).
    return Def->NumInputs >= 1 ? traceValueVA(Def->Inputs[0], Depth + 1)
                               : std::nullopt;
  case NdOp::SUBBYTES:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
        Def->Inputs[1].ConstVal == 0)
      if (auto B = traceValueVA(Def->Inputs[0], Depth + 1))
        return mask(*B, Def->Output.Size);
    return std::nullopt;
  case NdOp::INT_ADD:
    if (Def->NumInputs >= 2)
      if (auto A = traceValueVA(Def->Inputs[0], Depth + 1))
        if (auto B = traceValueVA(Def->Inputs[1], Depth + 1))
          return mask(*A + *B, Def->Output.Size);
    return std::nullopt;
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2)
      if (auto A = traceValueVA(Def->Inputs[0], Depth + 1))
        if (auto B = traceValueVA(Def->Inputs[1], Depth + 1))
          return mask(*A - *B, Def->Output.Size);
    return std::nullopt;
  default:
    return std::nullopt;
  }
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
