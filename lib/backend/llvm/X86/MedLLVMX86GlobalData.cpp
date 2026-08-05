//===- MedLLVMX86GlobalData.cpp - i386 PIC address recognizers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// i386 (32-bit x86) position-independent-code address recognizers for
/// MedLLVMEmitter's writable/code-pointer resolution.  These are the only
/// hard architecture-gated members of the global-data resolver (each returns
/// early unless TargetArch == Arch::X86), so they live here following the
/// target-dispatch split used by the other X86/ emitters rather than in the
/// architecture-neutral MedLLVMGlobalData.cpp:
///
///   - i386WalkedPointerDeref / funcUsesI386WalkedPointerDeref: a stack-spilled
///     pointer walked across loop iterations (`q++` search family);
///   - i386PeeledInitStoreAddr: the -O2 unrolled init-loop store address whose
///     PHI is the segment-offset induction base;
///   - i386WritableSegBasePlusOff: a GOTOFF-folded `base_const + off` writable
///     address, handing back the in-segment field displacement.
///
/// They call only other MedLLVMEmitter members (declared in the header) plus
/// getTargetRegInfo, so this is a pure translation-unit split; the shared
/// addrHasSymbolizedSegConst helper and the arch-neutral resolvers stay in
/// MedLLVMGlobalData.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

bool MedLLVMEmitter::i386WalkedPointerDeref(const MedVar &AddrVar) const {
  if (TargetArch != Arch::X86 || !CurMedFunc || AddrVar.isConst())
    return false;
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };
  unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
  // Segment-relative stack spill only (StoreSymbolized), not raw-VA rebase
  // slots.
  auto reloadIsSegmentRelative = [&](const MedOp *Def) -> bool {
    if (Def->Opcode != NdOp::LOAD || Def->NumInputs < 1)
      return false;
    auto LKey = addrSlotKey(Def->Inputs[0]);
    if (!LKey || stackSlotAddressEscapes(Def->Inputs[0]))
      return false;
    for (const auto &B : CurMedFunc->Blocks)
      for (const auto &O : B.Ops) {
        if (O.Opcode != NdOp::STORE || O.NumInputs < 2)
          continue;
        auto SKey = addrSlotKey(O.Inputs[0]);
        if (!SKey || *SKey != *LKey || PtrSz == 0)
          continue;
        if (!O.Inputs[1].isConst() && O.Inputs[1].Size != PtrSz)
          continue;
        if (O.Inputs[1].isConst()) {
          if (symbolizesWritableRelocPtr(O.Inputs[1].ConstVal,
                                         O.Inputs[1].Size))
            return true;
          continue;
        }
        auto StoreVA = traceValueVA(O.Inputs[1]);
        bool StoreSymbolized =
            StoreVA && symbolizesWritableRelocPtr(*StoreVA, O.Inputs[1].Size);
        if (!StoreSymbolized && varIsFrameDerived(O.Inputs[0]) &&
            frameSlotHasMatchingKeyLoad(O.Inputs[0]))
          continue;
        bool StoreSeg =
            writableDataSegOf(O.Inputs[1], /*RequireRelocBase=*/true);
        if (!StoreSeg && !O.Inputs[1].isConst())
          StoreSeg =
              writableDataSegOf(O.Inputs[1], /*RequireRelocBase=*/false) != 0;
        if (StoreSymbolized || StoreSeg)
          return true;
      }
    return false;
  };
  std::set<std::tuple<int, int, int>> Seen;
  std::vector<MedVar> Work{AddrVar};
  int Budget = 256;
  while (!Work.empty() && Budget-- > 0) {
    MedVar Cur = Work.back();
    Work.pop_back();
    if (Cur.isConst())
      continue;
    if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
      continue;
    const MedOp *Def = findDef(Cur);
    if (!Def) {
      if (const PhiNode *Phi = findPhi(Cur))
        for (const auto &[PredId, Arg] : Phi->Args) {
          (void)PredId;
          Work.push_back(Arg);
        }
      continue;
    }
    if (reloadIsSegmentRelative(Def))
      return true;
    if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
         Def->Opcode == NdOp::INT_SEXT) &&
        Def->NumInputs >= 1)
      Work.push_back(Def->Inputs[0]);
    else if (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs >= 2 &&
             Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0)
      Work.push_back(Def->Inputs[0]);
    else if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
             Def->NumInputs >= 2) {
      Work.push_back(Def->Inputs[0]);
      Work.push_back(Def->Inputs[1]);
    }
  }
  return false;
}

bool MedLLVMEmitter::funcUsesI386WalkedPointerDeref() const {
  if (TargetArch != Arch::X86 || !CurMedFunc)
    return false;
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          i386WalkedPointerDeref(Op.Inputs[0]))
        return true;
  return false;
}

bool MedLLVMEmitter::i386PeeledInitStoreAddr(const MedVar &AddrVar,
                                             uint64_t SegVA) const {
  if (TargetArch != Arch::X86 || !CurMedFunc || !Img || AddrVar.isConst())
    return false;
  if (!funcUsesI386WalkedPointerDeref())
    return false;
  if (i386WalkedPointerDeref(AddrVar))
    return false;
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };
  auto isUnrollPeel = [](int64_t C) -> bool {
    return C == -12 || C == -8 || C == -4 || C == 0;
  };
  auto peelInitInductionPhi = [&](const MedVar &V) -> bool {
    std::set<std::tuple<int, int, int>> Seen;
    std::vector<MedVar> Work{V};
    auto tracesToSegEntry = [&](const MedVar &Arg, auto &Self,
                                int Depth) -> bool {
      if (Depth > 8)
        return false;
      if (writableDataSegOf(Arg, /*RequireRelocBase=*/false) ||
          addrHasSymbolizedSegConst(Arg, SegVA))
        return true;
      if (const MedOp *AD = findDef(Arg)) {
        if ((AD->Opcode == NdOp::INT_ZEXT || AD->Opcode == NdOp::COPY ||
             AD->Opcode == NdOp::SUBBYTES) &&
            AD->NumInputs >= 1)
          return Self(AD->Inputs[0], Self, Depth + 1);
      }
      return false;
    };
    auto tracesToUnrollStep = [&](const MedVar &Arg, auto &Self,
                                  int Depth) -> bool {
      if (Depth > 8)
        return false;
      if (const MedOp *AD = findDef(Arg)) {
        if (AD->Opcode == NdOp::INT_ZEXT && AD->NumInputs >= 1)
          return Self(AD->Inputs[0], Self, Depth + 1);
        if (AD->Opcode == NdOp::INT_ADD && AD->NumInputs >= 2 &&
            AD->Inputs[1].isConst() && AD->Inputs[1].ConstVal == 0x10)
          return true;
      }
      return false;
    };
    while (!Work.empty()) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst())
        continue;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Phi = findPhi(Cur)) {
        bool HasSegEntry = false;
        bool HasUnrollStep = false;
        for (const auto &[PredId, Arg] : Phi->Args) {
          (void)PredId;
          if (tracesToSegEntry(Arg, tracesToSegEntry, 0))
            HasSegEntry = true;
          if (tracesToUnrollStep(Arg, tracesToUnrollStep, 0))
            HasUnrollStep = true;
        }
        return HasSegEntry && HasUnrollStep;
      }
      if (const MedOp *D = findDef(Cur)) {
        if ((D->Opcode == NdOp::COPY || D->Opcode == NdOp::INT_ZEXT ||
             D->Opcode == NdOp::SUBBYTES) &&
            D->NumInputs >= 1)
          Work.push_back(D->Inputs[0]);
      }
    }
    return false;
  };
  auto resolvePeelBase = [&](const MedVar &V) -> std::optional<MedVar> {
    std::set<std::tuple<int, int, int>> Seen;
    std::vector<MedVar> Work{V};
    while (!Work.empty()) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst())
        continue;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (findPhi(Cur))
        return Cur;
      if (const MedOp *D = findDef(Cur)) {
        if (D->Opcode == NdOp::COPY && D->NumInputs >= 1)
          Work.push_back(D->Inputs[0]);
      }
    }
    return std::nullopt;
  };

  const MedOp *D = findDef(AddrVar);
  std::optional<MedVar> Base;
  if (!D) {
    if (!findPhi(AddrVar))
      return false;
    Base = AddrVar;
  } else if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
    Base = resolvePeelBase(D->Inputs[0]);
  } else if (D->Opcode == NdOp::INT_ADD && D->NumInputs >= 2) {
    if (D->Inputs[1].isConst() &&
        isUnrollPeel(static_cast<int64_t>(D->Inputs[1].ConstVal)))
      Base = resolvePeelBase(D->Inputs[0]);
    else if (D->Inputs[0].isConst() &&
             isUnrollPeel(static_cast<int64_t>(D->Inputs[0].ConstVal)))
      Base = resolvePeelBase(D->Inputs[1]);
    else
      return false;
  } else {
    return false;
  }
  if (!Base)
    return false;
  return peelInitInductionPhi(*Base) &&
         addrHasSymbolizedSegConst(AddrVar, SegVA);
}

bool MedLLVMEmitter::i386WritableSegBasePlusOff(const MedVar &AddrVar,
                                                uint64_t SegVA, MedVar &OffVar,
                                                uint64_t &BaseConstVA) const {
  if (TargetArch != Arch::X86 || !CurMedFunc || !Img || AddrVar.isConst())
    return false;
  if (i386WalkedPointerDeref(AddrVar))
    return false;
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  const MedOp *D = findDef(AddrVar);
  if (!D || D->Opcode != NdOp::INT_ADD || D->NumInputs < 2)
    return false;
  const MedVar *BaseC = nullptr;
  const MedVar *Off = nullptr;
  if (D->Inputs[1].isConst()) {
    BaseC = &D->Inputs[1];
    Off = &D->Inputs[0];
  } else if (D->Inputs[0].isConst()) {
    BaseC = &D->Inputs[0];
    Off = &D->Inputs[1];
  } else {
    return false;
  }
  if (!BaseC->isConst())
    return false;
  // The base const is the GOTOFF-folded address `seg_VA + field_off` (GOT base
  // folds to 0).  It must actually land in the symbolized writable segment, or
  // the field displacement we hand back would be relative to the wrong base.
  const Segment *S = Img->getSegmentFor(BaseC->ConstVal);
  if (!S || S->VA != SegVA)
    return false;
  const MedOp *OffDef = findDef(*Off);
  if (!OffDef || OffDef->Opcode != NdOp::INT_ADD || OffDef->NumInputs < 2)
    return false;
  if (OffDef->Inputs[0].isConst() || OffDef->Inputs[1].isConst())
    return false;
  OffVar = *Off;
  BaseConstVA = BaseC->ConstVal;
  return addrHasSymbolizedSegConst(AddrVar, SegVA);
}

} // namespace neverd
