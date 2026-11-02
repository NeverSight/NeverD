//===- MedDispatchSlots.cpp - Selectors of a reloaded dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Selector recovery for a jump-table dispatch whose target travels through a
/// frame slot: tracing the target to the scaled index of its table load, in
/// the dispatch block and in each -O0 goto site that spills it, folding a
/// constant-index table load to its entry, and the per-predecessor table-base
/// check that keeps one shared dispatch from merging two tables.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedDispatchSlots.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/loader/BinaryImageModel.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace neverd {

std::optional<MedSlotKey> medAddressSlotKey(const MedVar &V,
                                            const MedDefLookup &DefOf,
                                            int Depth, bool ThroughRegs) {
  if (V.isConst() || Depth > limits::kMaxStackPtrTraceDepth)
    return std::nullopt;
  // A register anchors the key (frame/stack pointer base); never trace past it,
  // so a load and store of the same slot share an identical base.  ThroughRegs
  // threads only the TOP-LEVEL register's definition (escape detection): a slot
  // address copied into a parameter register before a call keys to the same
  // slot while the inner stack/frame-pointer register still anchors (threading
  // it would diverge from the load/store form via prologue self-copies).
  if (V.Kind == MedVar::Reg && !(ThroughRegs && Depth == 0))
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  if (!DefOf)
    return std::nullopt;
  const MedOp *Def = DefOf(V);
  if (!Def || Def->NumInputs < 1 ||
      Def->RegistrationRoot != MedOp::RegistrationRootKind::None)
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  // Slot identity follows the guest operation's modular width.  Accumulate as
  // raw bits so 64-bit wrap is defined and narrow guests do not become i64.
  auto applyConstantOffset = [&](const auto &Base, uint64_t Constant,
                                 bool Subtract) -> std::optional<MedSlotKey> {
    const unsigned Bytes = Def->Output.Size;
    if (Bytes == 0 || Bytes > sizeof(uint64_t))
      return std::nullopt;
    uint64_t Bits = std::bit_cast<uint64_t>(Base.second);
    Bits = Subtract ? Bits - Constant : Bits + Constant;
    if (Bytes < sizeof(uint64_t)) {
      const unsigned Width = Bytes * 8;
      const uint64_t Mask = (uint64_t{1} << Width) - 1;
      Bits &= Mask;
      if (Bits & (uint64_t{1} << (Width - 1)))
        Bits |= ~Mask;
    }
    return std::make_pair(Base.first, std::bit_cast<int64_t>(Bits));
  };
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return medAddressSlotKey(Def->Inputs[0], DefOf, Depth + 1, ThroughRegs);
  case NdOp::SUBBYTES:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
        Def->Inputs[1].ConstVal == 0)
      return medAddressSlotKey(Def->Inputs[0], DefOf, Depth + 1, ThroughRegs);
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  case NdOp::INT_ADD:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst()) {
      if (auto B =
              medAddressSlotKey(Def->Inputs[0], DefOf, Depth + 1, ThroughRegs))
        return applyConstantOffset(*B, Def->Inputs[1].ConstVal,
                                   /*Subtract=*/false);
    }
    if (Def->NumInputs >= 2 && Def->Inputs[0].isConst()) {
      if (auto B =
              medAddressSlotKey(Def->Inputs[1], DefOf, Depth + 1, ThroughRegs))
        return applyConstantOffset(*B, Def->Inputs[0].ConstVal,
                                   /*Subtract=*/false);
    }
    return std::nullopt;
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst()) {
      if (auto B =
              medAddressSlotKey(Def->Inputs[0], DefOf, Depth + 1, ThroughRegs))
        return applyConstantOffset(*B, Def->Inputs[1].ConstVal,
                                   /*Subtract=*/true);
    }
    return std::nullopt;
  default:
    return std::make_pair(std::make_pair(V.Id, V.SSAVer), int64_t{0});
  }
}

std::optional<MedVar> findTableSwitchIndex(const MedDispatchContext &C,
                                           const MedBlock &Blk,
                                           const MedVar &Target) {
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (auto &Op : Blk.Ops)
    if (!Op.Output.isConst() && Op.Output.Size > 0)
      Defs[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  std::vector<MedVar> Work{Target};
  std::set<std::pair<int, int>> Seen;
  int Steps = 0;
  while (!Work.empty() &&
         Steps < ::neverd::limits::ir::
                     kMedDispatchSlotsFindTableSwitchIndexStepsLimit) {
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
    if (Op->Opcode == NdOp::INTRINSIC && Op->NumInputs &&
        Op->Inputs[0].isConst() &&
        isAArch64AddressMemoryIntrinsic(
            static_cast<Intrinsic>(Op->Inputs[0].ConstVal))) {
      if (!C.Func || !C.Img || C.Img->Arch != Arch::AArch64 ||
          !aarch64AddressLoadMedShapeIsValid(*Op))
        return std::nullopt;
      const auto Instruction = aarch64AddressMemoryInstruction(
          Op->Inputs[kAddressMemoryWordOperand + 1].ConstVal);
      if (Instruction->stackBase() || Instruction->isVector() ||
          (uint64_t(1) << Instruction->indexShift()) !=
              Instruction->accessBytes())
        return std::nullopt;
      // The raw index is already the physical slot coordinate. Reuse it only
      // for a resolver-certified read, retaining the original load.
      // Never trace into its base as though that were a complete address.
      size_t Tables = 0;
      for (const JumpTable &JT : C.Func->JumpTables) {
        if (!Blk.Ops.empty() && Blk.Ops.back().Opcode == NdOp::INDIR_BR &&
            JT.InsnAddr != Blk.Ops.back().Addr)
          continue;
        if (JT.MutatedUnsafe || JT.PreScaledIndex || JT.TwoLevelIndex ||
            JT.TwoTableSelect || !JT.HasDispatchSlotMap ||
            JT.EntrySize != Instruction->accessBytes() ||
            (JT.EntryStride && JT.EntryStride != Instruction->accessBytes()))
          continue;
        const auto Matches =
            std::count_if(JT.AuthenticatedTableLoads.begin(),
                          JT.AuthenticatedTableLoads.end(),
                          [&](const JumpTableOpOccurrence &Occurrence) {
                            return Occurrence.Addr == Op->Addr &&
                                   Occurrence.Seq == Op->OriginSeq &&
                                   Occurrence.Size == Op->Output.Size;
                          });
        Tables += Matches == 1;
      }
      size_t Occurrences = 0;
      for (const auto &Block : C.Func->Blocks)
        for (const auto &Candidate : Block.Ops)
          if (Candidate.Addr == Op->Addr &&
              Candidate.OriginSeq == Op->OriginSeq &&
              aarch64AddressLoadMedShapeIsValid(Candidate))
            ++Occurrences;
      if (Tables != 1 || Occurrences != 1)
        return std::nullopt;
      return Op->Inputs[kAddressMemoryIndexOperand + 1];
    }
    if (Op->Opcode == NdOp::INT_MULT || Op->Opcode == NdOp::INT_LEFT) {
      for (int I = 0; I < Op->NumInputs; ++I)
        if (!Op->Inputs[I].isConst())
          return Op->Inputs[I];
      continue;
    }
    // A peeled first switch iteration computes the table-loaded target, spills
    // it to a fixed stack slot, then reloads it for the indirect branch.  The
    // address trace alone stops at the frame pointer; forward through the spill
    // (the STORE writing the same frame slot) to reach the table load and
    // its scaled index.  The slot key covers only `base+const` slots, so a
    // genuine table load (`base + idx*scale`) never matches a store and falls
    // through to the address trace below.
    if (Op->Opcode == NdOp::LOAD && Op->NumInputs >= 1) {
      if (auto LKey = C.SlotKeyOf(Op->Inputs[Op->NumInputs >= 2 ? 1 : 0])) {
        for (auto &SOp : Blk.Ops)
          if (SOp.Opcode == NdOp::STORE && SOp.NumInputs >= 2)
            if (auto SKey = C.SlotKeyOf(SOp.Inputs[0]);
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

std::optional<MedSlotKey> dispatchReloadSlot(const MedDispatchContext &C,
                                             const MedBlock &Blk,
                                             const MedOp &BrOp) {
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
      return C.SlotKeyOf(D->Inputs[D->NumInputs >= 2 ? 1 : 0]);
    if (D->NumInputs >= 1 && !D->Inputs[0].isConst()) {
      V = D->Inputs[0];
      continue;
    }
    break;
  }
  return std::nullopt;
}

std::optional<MedVar> predecessorSwitchIndex(const MedDispatchContext &C,
                                             const MedBlock &Pred,
                                             const MedSlotKey &SlotKey) {
  // The predecessor's store to the same slot carries the table-loaded target;
  // the last such store before the branch is the value the reload observes.
  MedVar Stored;
  bool Found = false;
  for (auto &Op : Pred.Ops)
    if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
      if (auto SKey = C.SlotKeyOf(Op.Inputs[0]); SKey && *SKey == SlotKey) {
        Stored = Op.Inputs[1];
        Found = true;
      }
  if (!Found || Stored.isConst())
    return std::nullopt;
  // Reuse the in-block tracer on the predecessor: the stored value is the table
  // load, so this resolves to its scaled index.
  return findTableSwitchIndex(C, Pred, Stored);
}

std::optional<MedVar> constTableLoadIndex(const MedDispatchContext &C,
                                          const MedVar &Val,
                                          const MedDefLookup &defOf,
                                          const JumpTable &JT,
                                          bool RequireAuthenticatedLoad) {
  if (JT.EntrySize == 0 || !JT.HasBaseAddr || JT.Targets.empty() ||
      Val.isConst() || !JT.HasDispatchSlotMap ||
      JT.SlotIndices.size() != JT.Targets.size())
    return std::nullopt;
  const std::set<uint32_t> UniqueSlots(JT.SlotIndices.begin(),
                                       JT.SlotIndices.end());
  if (UniqueSlots.size() != JT.SlotIndices.size())
    return std::nullopt;
  const uint64_t PhysicalStride =
      JT.EntryStride != 0 ? JT.EntryStride : JT.EntrySize;
  if (PhysicalStride < JT.EntrySize)
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
  if (!Load || Load->NumInputs < 1 || Load->Output.Size != JT.EntrySize)
    return std::nullopt;

  if (RequireAuthenticatedLoad) {
    // A missing exact selector may be replaced only by the one surviving
    // resolver-authenticated table read. Addr alone is not an occurrence
    // identity, and duplicated Med occurrences are deliberately ambiguous.
    const auto AuthenticatedMatches = std::count_if(
        JT.AuthenticatedTableLoads.begin(), JT.AuthenticatedTableLoads.end(),
        [&](const JumpTableOpOccurrence &Occurrence) {
          return Occurrence.Addr == Load->Addr &&
                 Occurrence.Seq == Load->OriginSeq &&
                 Occurrence.Size == Load->Output.Size;
        });
    if (AuthenticatedMatches != 1 || !C.Func)
      return std::nullopt;
    size_t SurvivingMatches = 0;
    bool CurrentLoadIsUniqueMatch = false;
    for (const MedBlock &Block : C.Func->Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.Addr == Load->Addr &&
            Op.OriginSeq == Load->OriginSeq &&
            Op.Output.Size == Load->Output.Size) {
          ++SurvivingMatches;
          CurrentLoadIsUniqueMatch = &Op == Load;
        }
    if (SurvivingMatches != 1 || !CurrentLoadIsUniqueMatch)
      return std::nullopt;
  }

  // Decompose the load address into an optional single opaque base subtree plus
  // a sum of constant addends, rejecting any scaled variable index.  Pure
  // pass-through renames (COPY / width casts) are skipped iteratively so a long
  // -O0 SSA-via-memory reload chain (clang materialises the ARM32 PC-relative
  // table base through dozens of COPYs) does not exhaust the addend-tree depth
  // budget.  The single opaque base (if present) is captured so its runtime
  // contribution can be modelled below; a genuine variable-index load carries a
  // scaled index (idx*scale) and is rejected so it falls through to the
  // variable-index recovery / loud trap.
  const uint16_t AddressBytes = C.AddressBytes;
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
    for (int Hop = 0;
         Hop < ::neverd::limits::ir::kMedDispatchSlotsSkipPassThroughHopLimit;
         ++Hop) {
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
    // An exact LowIR GOTOFF certificate models the selected i386 GOT base as
    // zero in relocatable-object coordinates.  Preserve that authoritative
    // model while folding a constant table slot; descending into the raw
    // call/pop arithmetic would instead reintroduce its architectural PC.
    if (C.ModelZero(V))
      return;
    V = skipPassThrough(V);
    if (C.ModelZero(V))
      return;
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
    if (C.Img) {
      const MedOp *BD = defOf(OpaqueBase);
      if (BD && BD->Opcode == NdOp::LOAD && BD->NumInputs >= 1) {
        MedVar AddrV = skipPassThrough(BD->Inputs[0]);
        if (AddrV.isConst()) {
          uint32_t W = C.Img->getPointerSize();
          if (const uint8_t *P = C.Img->readVA(AddrV.ConstVal, W)) {
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

std::optional<MedVar> predecessorConstDispatchIndex(const MedDispatchContext &C,
                                                    const MedBlock &Pred,
                                                    const MedSlotKey &SlotKey,
                                                    const JumpTable &JT) {
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
      if (auto SKey = C.SlotKeyOf(Op.Inputs[0]); SKey && *SKey == SlotKey) {
        Stored = Op.Inputs[1];
        Found = true;
      }
  if (!Found)
    return std::nullopt;
  return constTableLoadIndex(C, Stored, defOf, JT,
                             /*RequireAuthenticatedLoad=*/false);
}

std::optional<uint64_t> predecessorTableBase(const MedDispatchContext &C,
                                             const MedBlock &Pred,
                                             const MedSlotKey &SlotKey) {
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
      if (auto SKey = C.SlotKeyOf(Op.Inputs[0]); SKey && *SKey == SlotKey) {
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
      const Segment *S = C.Img ? C.Img->getSegmentFor(X.ConstVal) : nullptr;
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

std::optional<MedSharedDispatchSelectors>
sharedDispatchSelectors(const MedDispatchContext &C, const MedBlock &Blk,
                        const MedOp &BrOp, const JumpTable &JT) {
  if (!C.Func || Blk.Preds.size() < 2)
    return std::nullopt;
  auto SlotKey = dispatchReloadSlot(C, Blk, BrOp);
  if (!SlotKey)
    return std::nullopt;

  // Recover every predecessor's index.  Bail (returning nothing) if any cannot
  // be resolved.  Predecessors indexing DIFFERENT tables (clang -O0 funnels
  // every `goto *p` in a function through one dispatch block, so two distinct
  // tables `t1[i]`/`t2[j]` can share it) would make the common-index merge
  // unsound; that case is detected and invalidated upstream in the resolver
  // (where table bases are robustly resolved cross-arch via foldRegConstant),
  // so a mixed-table dispatch never reaches here with targets.
  std::vector<std::pair<int, MedVar>> PerPred;
  unsigned MaxBytes = 0;
  std::optional<uint64_t> CommonBase;
  for (int PredId : Blk.Preds) {
    const MedBlock *Pred = nullptr;
    for (auto &B : C.Func->Blocks)
      if (B.Id == PredId) {
        Pred = &B;
        break;
      }
    if (!Pred)
      return std::nullopt;
    auto Idx = predecessorSwitchIndex(C, *Pred, *SlotKey);
    if (Idx) {
      // A variable-index goto-site: enforce a single shared table.  Bail on a
      // POSITIVE table-base mismatch (two predecessors resolve to different
      // data-segment table VAs → different tables → a common-index merge would
      // mis-route, so trap loudly instead).  When a base cannot be resolved to
      // a data-segment constant (e.g. an ARM PC-relative literal-pool base),
      // proceed: the common single-table threaded-dispatch shape must keep
      // working.
      if (auto Base = predecessorTableBase(C, *Pred, *SlotKey)) {
        if (!CommonBase)
          CommonBase = Base;
        else if (*CommonBase != *Base)
          return std::nullopt;
      }
    } else {
      // A constant-index goto-site (`goto *tab[k]`, clang -O0 folds it to a
      // load from a constant slot address): recover the exact entry against
      // THIS table.  No base check is needed — the constant address resolves to
      // one table entry, and a constant into a different table maps out of
      // range (or misaligned) and bails, so it cannot mis-route.
      Idx = predecessorConstDispatchIndex(C, *Pred, *SlotKey, JT);
      if (!Idx)
        return std::nullopt;
    }
    MaxBytes = std::max<unsigned>(MaxBytes, Idx->Size ? Idx->Size : 4);
    PerPred.emplace_back(PredId, *Idx);
  }
  if (PerPred.size() < 2 || MaxBytes == 0)
    return std::nullopt;
  return MedSharedDispatchSelectors{std::move(PerPred), MaxBytes};
}

} // namespace neverd
