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
