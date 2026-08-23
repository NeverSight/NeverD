//===- MedLLVMSwitchIndex.cpp - Switch index recovery ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Switch-index recovery for MedLLVMEmitter: tracing an INDIR_BR target
/// back to the scaled index of its table load, in-block and across the
/// frame spill of an -O0 computed-goto dispatch, plus the constant-index
/// table-load folding and the per-predecessor table-base check that keeps
/// a shared multi-site dispatch from merging two distinct tables.  The
/// recovered index is consumed by the switch lowering in
/// MedLLVMSwitch.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedSwitchNorm.h"

#define DEBUG_TYPE "neverd-med-llvm-switch"
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
#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

/// Trace an INDIR_BR target var back to the scaled index of its table load.
/// The index is the non-constant operand of the INT_MULT/INT_LEFT that scales
/// it, so this skips the table base and finds the genuine switch variable.
std::optional<MedVar>
MedLLVMEmitter::findSwitchIndex(const MedBlock &Blk,
                                const MedVar &Target) const {
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &Op : Blk.Ops)
    if (!Op.Output.isConst() && Op.Output.Size > 0)
      Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  std::vector<MedVar> Work{Target};
  std::set<std::pair<int, int>> Seen;
  int Steps = 0;
  while (!Work.empty() && Steps < 256) {
    ++Steps;
    MedVar V = Work.back();
    Work.pop_back();
    if (V.isConst())
      continue;
    if (!Seen.insert({V.Id, V.SSAVer}).second)
      continue;
    auto It = Defs.find({V.Id, V.SSAVer});
    if (It == Defs.end())
      continue;
    const MedOp *Op = It->second;
    if (Op->Opcode == NdOp::INT_MULT || Op->Opcode == NdOp::INT_LEFT) {
      for (int I = 0; I < Op->NumInputs; ++I)
        if (!Op->Inputs[I].isConst())
          return Op->Inputs[I];
      continue;
    }
    // A peeled first switch iteration computes the table-loaded target, spills
    // it to a fixed stack slot, then reloads it for the indirect branch.  The
    // address trace alone stops at the frame pointer; forward through the spill
    // (the STORE writing the same addrSlotKey slot) to reach the table load and
    // its scaled index.  addrSlotKey keys only `base+const` slots, so a genuine
    // table load (`base + idx*scale`) never matches a store and falls through
    // to the address trace below.
    if (Op->Opcode == NdOp::LOAD && Op->NumInputs >= 1) {
      if (auto LKey = addrSlotKey(Op->Inputs[Op->NumInputs >= 2 ? 1 : 0])) {
        for (auto &SOp : Blk.Ops)
          if (SOp.Opcode == NdOp::STORE && SOp.NumInputs >= 2)
            if (auto SKey = addrSlotKey(SOp.Inputs[0]);
                SKey && *SKey == *LKey) {
              Work.push_back(SOp.Inputs[1]);
              break;
            }
      }
    }
    for (int I = 0; I < Op->NumInputs; ++I)
      if (!Op->Inputs[I].isConst())
        Work.push_back(Op->Inputs[I]);
  }
  return std::nullopt;
}

std::optional<std::pair<std::pair<int, int>, int64_t>>
MedLLVMEmitter::reloadSlotKeyOf(const MedBlock &Blk, const MedOp &BrOp) const {
  if (BrOp.NumInputs < 1 || BrOp.Inputs[0].isConst())
    return std::nullopt;
  // Build local defs for the dispatch block so the branch target can be traced
  // to the frame reload it observes.
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &Op : Blk.Ops)
    if (!Op.Output.isConst() && Op.Output.Size > 0)
      Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  MedVar V = BrOp.Inputs[0];
  for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
    if (V.isConst())
      break;
    auto It = Defs.find({V.Id, V.SSAVer});
    if (It == Defs.end())
      break;
    const MedOp *D = It->second;
    if (D->Opcode == NdOp::LOAD && D->NumInputs >= 1)
      return addrSlotKey(D->Inputs[D->NumInputs >= 2 ? 1 : 0]);
    if (D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
      V = D->Inputs[0];
      continue;
    }
    break;
  }
  return std::nullopt;
}

std::optional<MedVar> MedLLVMEmitter::tracePredSwitchIndex(
    const MedBlock &Pred,
    const std::pair<std::pair<int, int>, int64_t> &SlotKey) const {
  // The predecessor's store to the same slot carries the table-loaded target;
  // the last such store before the branch is the value the reload observes.
  MedVar Stored;
  bool Found = false;
  for (auto &Op : Pred.Ops)
    if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
      if (auto SKey = addrSlotKey(Op.Inputs[0]); SKey && *SKey == SlotKey) {
        Stored = Op.Inputs[1];
        Found = true;
      }
  if (!Found || Stored.isConst())
    return std::nullopt;
  // Reuse the in-block tracer on the predecessor: the stored value is the table
  // load, so this resolves to its scaled index.
  return findSwitchIndex(Pred, Stored);
}

std::optional<MedVar> MedLLVMEmitter::constIndexFromTableLoad(
    const MedVar &Val,
    const std::function<const MedOp *(const MedVar &)> &defOf,
    const JumpTable &JT) const {
  if (JT.EntrySize == 0 || !JT.HasBaseAddr || JT.Targets.empty() ||
      Val.isConst())
    return std::nullopt;

  // Trace the value to the LOAD that read the table entry.
  const MedOp *Load = nullptr;
  {
    MedVar V = Val;
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      const MedOp *D = defOf(V);
      if (!D)
        break;
      if (D->Opcode == NdOp::LOAD) {
        Load = D;
        break;
      }
      if (D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
        V = D->Inputs[0];
        continue;
      }
      break;
    }
  }
  if (!Load || Load->NumInputs < 1)
    return std::nullopt;

  // Decompose the load address into an optional single opaque base subtree plus
  // a sum of constant addends, rejecting any scaled variable index.  Pure
  // pass-through renames (COPY / width casts) are skipped iteratively so a long
  // -O0 SSA-via-memory reload chain (clang materialises the ARM32 PC-relative
  // table base through dozens of COPYs) does not exhaust the addend-tree depth
  // budget.  The single opaque base (if present) is captured so its runtime
  // contribution can be modelled below; a genuine variable-index load carries a
  // scaled index (idx*scale) and is rejected so it falls through to the
  // variable-index recovery / loud trap.
  const uint16_t AddressBytes =
      Img && Img->getPointerSize() != 0
          ? static_cast<uint16_t>(Img->getPointerSize())
          : getTargetRegInfo(TargetArch).PointerSize;
  const unsigned AddressBits =
      AddressBytes > 0 && AddressBytes < sizeof(uint64_t)
          ? unsigned(AddressBytes) * 8u
          : 64u;
  const uint64_t AddressMask = AddressBits == 64
                                   ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t(1) << AddressBits) - 1;
  uint64_t ConstSum = 0;
  int BaseCount = 0;
  bool HasScaledIndex = false;
  bool TooDeep = false;
  MedVar OpaqueBase;
  auto skipPassThrough = [&](MedVar V) -> MedVar {
    for (int Hop = 0; Hop < 256; ++Hop) {
      if (V.isConst())
        return V;
      const MedOp *D = defOf(V);
      if (D &&
          (D->Opcode == NdOp::COPY || D->Opcode == NdOp::INT_ZEXT ||
           D->Opcode == NdOp::INT_SEXT) &&
          D->NumInputs >= 1)
        V = D->Inputs[0];
      else
        return V;
    }
    return V;
  };
  auto accumulate = [&](uint64_t Value, int Sign) {
    ConstSum = Sign > 0 ? ConstSum + Value : ConstSum - Value;
    ConstSum &= AddressMask;
  };
  std::function<void(MedVar, int, int)> walk = [&](MedVar V, int Depth,
                                                   int Sign) {
    if (Depth > limits::kMaxQuasiCopyDepth) {
      TooDeep = true;
      return;
    }
    V = skipPassThrough(V);
    if (V.isConst()) {
      accumulate(V.ConstVal & AddressMask, Sign);
      return;
    }
    const MedOp *D = defOf(V);
    if (!D || D->NumInputs < 1) {
      if (Sign < 0) {
        TooDeep = true; // subtraction of an opaque base has no supported model
        return;
      }
      ++BaseCount; // opaque base (PIC base, parameter, cross-scope value)
      OpaqueBase = V;
      return;
    }
    switch (D->Opcode) {
    case NdOp::INT_ADD:
      if (D->NumInputs < 2) {
        ++BaseCount;
        OpaqueBase = V;
        break;
      }
      walk(D->Inputs[0], Depth + 1, Sign);
      walk(D->Inputs[1], Depth + 1, Sign);
      break;
    case NdOp::INT_SUB:
      if (D->NumInputs < 2) {
        ++BaseCount;
        OpaqueBase = V;
        break;
      }
      walk(D->Inputs[0], Depth + 1, Sign);
      walk(D->Inputs[1], Depth + 1, -Sign);
      break;
    case NdOp::INT_MULT:
    case NdOp::INT_LEFT:
      if (D->NumInputs >= 2 && D->Inputs[0].isConst() &&
          D->Inputs[1].isConst()) {
        const uint64_t A = D->Inputs[0].ConstVal & AddressMask;
        const uint64_t B = D->Inputs[1].ConstVal;
        uint64_t Term = 0;
        if (D->Opcode == NdOp::INT_MULT)
          Term = A * B;
        else if (B < AddressBits)
          Term = A << unsigned(B);
        accumulate(Term & AddressMask, Sign);
      } else {
        HasScaledIndex = true; // a runtime-scaled index → variable dispatch
      }
      break;
    default:
      ++BaseCount; // any other producer is treated as an opaque base (e.g.
                   // LOAD)
      OpaqueBase = V;
      break;
    }
  };
  walk(Load->Inputs[Load->NumInputs >= 2 ? 1 : 0], 0, 1);
  if (TooDeep || HasScaledIndex || BaseCount > 1)
    return std::nullopt;

  // Recover the entry index K.  The entry's absolute VA is ConstSum plus the
  // runtime value B of the single opaque base (if any); K = (entryVA -
  // Base)/ES. B is unknown statically, so the three legal base models are
  // enumerated and we accept ONLY when exactly one yields an in-range,
  // ES-aligned index.  This is sound with no mis-routing risk: the table base
  // VA dwarfs the table size, so at most one model lands in range for any legal
  // form; a collision (two distinct in-range models) or a miss (none) falls
  // through to the loud trap rather than guess.  The models:
  //   * B == 0           — the base folds into ConstSum (AArch64 adrp / x86-64
  //                        RIP, BaseCount==0) or is a model-zero PIC/GOT base
  //                        register (i386 get_pc_thunk, BaseCount==1):
  //                        offset = ConstSum - JT.BaseAddr;
  //   * B == JT.BaseAddr — the resolved table VA itself sits in the opaque
  //                        register: offset = ConstSum;
  //   * B == *litpool    — the opaque base is a LOAD of a PC-relative table
  //                        displacement from a constant literal-pool VA
  //                        (ARM32): offset = ConstSum + *litpool - JT.BaseAddr.
  const uint64_t PhysicalStride =
      JT.EntryStride != 0 ? JT.EntryStride : JT.EntrySize;
  auto tryOffset = [&](uint64_t Off) -> std::optional<uint64_t> {
    Off &= AddressMask;
    if (PhysicalStride == 0 || Off % PhysicalStride != 0)
      return std::nullopt;
    const uint64_t PhysicalSlot = Off / PhysicalStride;
    return JT.targetPositionForPhysicalSlot(PhysicalSlot);
  };
  std::optional<uint64_t> Chosen;
  bool Ambiguous = false;
  auto consider = [&](std::optional<uint64_t> C) {
    if (!C)
      return;
    if (!Chosen)
      Chosen = C;
    else if (*Chosen != *C)
      Ambiguous = true; // two distinct in-range models → unresolvable
  };
  consider(tryOffset(ConstSum - (JT.BaseAddr & AddressMask))); // B==0
  if (BaseCount == 1) {
    consider(
        tryOffset(ConstSum)); // B == JT.BaseAddr (table VA in the register)
    // B == *litpool: the opaque base is a LOAD whose address folds to a
    // constant VA in a readable segment — the ARM32 PC-relative literal pool
    // word holding the table's PC-relative displacement.  Reading it yields the
    // exact base.
    if (Img) {
      const MedOp *BD = defOf(OpaqueBase);
      if (BD && BD->Opcode == NdOp::LOAD && BD->NumInputs >= 1) {
        MedVar AddrV = skipPassThrough(BD->Inputs[0]);
        if (AddrV.isConst()) {
          uint32_t W = Img->getPointerSize();
          if (const uint8_t *P = Img->readVA(AddrV.ConstVal, W)) {
            uint64_t Lit = 0;
            std::memcpy(&Lit, P, W);
            if (W > 0 && W < sizeof(uint64_t)) {
              const unsigned LitBits = W * 8u;
              const uint64_t LitMask = (uint64_t(1) << LitBits) - 1;
              Lit &= LitMask;
              if ((Lit & (uint64_t(1) << (LitBits - 1))) != 0)
                Lit |= ~LitMask;
            }
            consider(tryOffset(ConstSum + Lit - (JT.BaseAddr & AddressMask)));
          }
        }
      }
    }
  }
  if (Ambiguous || !Chosen)
    return std::nullopt; // unresolvable → loud trap (never mis-route)
  uint64_t K = *Chosen;

  // The switch case value for table entry K is its recovered real index label
  // (or K when labels are the identity); dispatching on it routes to the same
  // case block the variable-index sites reach.
  int64_t Val2 = (JT.CaseLabels.size() == JT.Targets.size())
                     ? JT.CaseLabels[K]
                     : static_cast<int64_t>(K);
  return MedVar::makeConst(static_cast<uint64_t>(Val2), 4);
}

std::optional<MedVar> MedLLVMEmitter::tracePredConstDispatchIndex(
    const MedBlock &Pred,
    const std::pair<std::pair<int, int>, int64_t> &SlotKey,
    const JumpTable &JT) const {
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &Op : Pred.Ops)
    if (!Op.Output.isConst() && Op.Output.Size > 0)
      Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Defs.find({V.Id, V.SSAVer});
    return It == Defs.end() ? nullptr : It->second;
  };

  // The predecessor's last store to the dispatch slot carries the constant
  // table-loaded target.
  MedVar Stored;
  bool Found = false;
  for (auto &Op : Pred.Ops)
    if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
      if (auto SKey = addrSlotKey(Op.Inputs[0]); SKey && *SKey == SlotKey) {
        Stored = Op.Inputs[1];
        Found = true;
      }
  if (!Found)
    return std::nullopt;
  return constIndexFromTableLoad(Stored, defOf, JT);
}

std::optional<MedVar>
MedLLVMEmitter::traceConstBranchIndex(const MedOp &BrOp,
                                      const JumpTable &JT) const {
  if (!CurMedFunc || BrOp.NumInputs < 1 || BrOp.Inputs[0].isConst())
    return std::nullopt;
  // The table read may sit in a dominating block (a hoisted load), so the def
  // map spans the whole function; the in-block tracers only see the dispatch
  // block.
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &B : CurMedFunc->Blocks)
    for (auto &Op : B.Ops)
      if (!Op.Output.isConst() && Op.Output.Size > 0)
        Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Defs.find({V.Id, V.SSAVer});
    return It == Defs.end() ? nullptr : It->second;
  };
  return constIndexFromTableLoad(BrOp.Inputs[0], defOf, JT);
}

std::optional<uint64_t> MedLLVMEmitter::predTableBaseVA(
    const MedBlock &Pred,
    const std::pair<std::pair<int, int>, int64_t> &SlotKey) const {
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &Op : Pred.Ops)
    if (!Op.Output.isConst() && Op.Output.Size > 0)
      Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.isConst())
      return nullptr;
    auto It = Defs.find({V.Id, V.SSAVer});
    return It == Defs.end() ? nullptr : It->second;
  };

  MedVar Stored;
  bool Found = false;
  for (auto &Op : Pred.Ops)
    if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
      if (auto SKey = addrSlotKey(Op.Inputs[0]); SKey && *SKey == SlotKey) {
        Stored = Op.Inputs[1];
        Found = true;
      }
  if (!Found || Stored.isConst())
    return std::nullopt;

  const MedOp *Load = nullptr;
  {
    MedVar V = Stored;
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      const MedOp *D = defOf(V);
      if (!D)
        break;
      if (D->Opcode == NdOp::LOAD) {
        Load = D;
        break;
      }
      if (D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
        V = D->Inputs[0];
        continue;
      }
      break;
    }
  }
  if (!Load || Load->NumInputs < 1)
    return std::nullopt;

  // Collect the largest constant addend in the load address that lands in a
  // non-executable data segment — the table base VA.  INT_LEFT/INT_MULT (the
  // scaled index) are not descended (so a modulo index's magic constants are
  // not picked up); a PC-relative base's PC constant lands in `.text`
  // (executable) and is excluded, so an ARM literal-pool base correctly yields
  // nullopt.
  std::optional<uint64_t> Base;
  std::function<void(MedVar, int)> walk = [&](MedVar X, int Depth) {
    if (Depth > limits::kMaxQuasiCopyDepth)
      return;
    if (X.isConst()) {
      const Segment *S = Img ? Img->getSegmentFor(X.ConstVal) : nullptr;
      if (S && !S->isExecutable() && (!Base || X.ConstVal > *Base))
        Base = X.ConstVal;
      return;
    }
    const MedOp *D = defOf(X);
    if (!D)
      return;
    switch (D->Opcode) {
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
      for (int I = 0; I < D->NumInputs; ++I)
        walk(D->Inputs[I], Depth + 1);
      break;
    default:
      break;
    }
  };
  walk(Load->Inputs[Load->NumInputs >= 2 ? 1 : 0], 0);
  return Base;
}

std::optional<MedVar>
MedLLVMEmitter::findSwitchIndexCrossBlock(const MedBlock &Blk,
                                          const MedOp &BrOp) const {
  if (!CurMedFunc)
    return std::nullopt;
  // Only a single-predecessor dispatch is unambiguous: the index is computed in
  // the one block that dominates the branch.  Multiple predecessors are routed
  // through synthesizeSharedDispatchIndex instead.
  if (Blk.Preds.size() != 1)
    return std::nullopt;
  auto SlotKey = reloadSlotKeyOf(Blk, BrOp);
  if (!SlotKey)
    return std::nullopt;
  const MedBlock *Pred = nullptr;
  for (auto &B : CurMedFunc->Blocks)
    if (B.Id == Blk.Preds[0]) {
      Pred = &B;
      break;
    }
  if (!Pred)
    return std::nullopt;
  // The index is defined in the dominating predecessor, so getVar in the
  // dispatch block reads it correctly via the alloca model.
  return tracePredSwitchIndex(*Pred, *SlotKey);
}

} // namespace neverd
