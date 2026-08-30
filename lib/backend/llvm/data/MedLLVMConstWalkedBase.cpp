//===- MedLLVMConstWalkedBase.cpp - Walked-base classification -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walked-base classification for MedLLVMEmitter: recognising a constant
/// that is the common base of a pointer DIFFERENCE, a self-advancing
/// induction PHI base, or the original VA of a rodata C-string an
/// induction pointer walks.  A walked base must keep one consistent
/// addressing model across every increment, so these gate the
/// symbolization predicates in MedLLVMConstClass.cpp.  Every routine here
/// is a MedLLVMEmitter member declared in the shared header, so this is a
/// pure translation-unit split.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool MedLLVMEmitter::valIsPointerDiffBase(uint64_t Val) const {
  if (!CurMedFunc || Val == 0)
    return false;
  auto findDef = [&](const MedVar &Y) { return lookupDef(Y); };
  auto findPhi = [&](const MedVar &Y) { return lookupPhi(Y); };
  // True when \p Start reaches the constant \p Val through the copy/width/add/
  // sub/select/phi chain a walked pointer threads its base through.
  auto reachesConst = [&](const MedVar &Start) {
    std::vector<MedVar> Work{Start};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 512;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        if (Cur.ConstVal == Val)
          return true;
        continue;
      }
      if (auto C = traceSSAConst(Cur))
        if (*C == Val)
          return true;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Ph = findPhi(Cur)) {
        for (const auto &[Pred, A] : Ph->Args) {
          if (!phiIncomingEdgeFeasible(*Ph, Pred))
            continue;
          Work.push_back(A);
        }
        continue;
      }
      if (const MedOp *D = findDef(Cur)) {
        if (auto Forwarded = pointerPreservingInput(*D)) {
          Work.push_back(*Forwarded);
          continue;
        }
        switch (D->Opcode) {
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
          for (int I = 0; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        case NdOp::SELECT:
          if (!selectPreservesPointerValues(*D))
            break;
          Work.push_back(D->Inputs[1]);
          Work.push_back(D->Inputs[2]);
          break;
        default:
          break;
        }
      }
    }
    return false;
  };
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::INT_SUB || Op.NumInputs < 2)
        continue;
      // Both operands of the subtraction trace back to the same constant base —
      // a genuine pointer difference, not `pointer - smallConst`.
      if (!Op.Inputs[0].isConst() && !Op.Inputs[1].isConst() &&
          reachesConst(Op.Inputs[0]) && reachesConst(Op.Inputs[1]))
        return true;
    }
  return false;
}

bool MedLLVMEmitter::valIsAdvancingInductionBase(uint64_t Val) const {
  if (!CurMedFunc || Val == 0)
    return false;
  // A walked pointer materializes its base as a folded INTERIOR constant
  // (`&G[k]` = segVA + k*stride, e.g. `q = arr+63`), distinct from the array
  // base \p Val used by direct `G[i]` accesses.  Symbolizing the base while the
  // walk threads the raw interior VA mixes models, so treat ANY constant in \p
  // Val's own writable segment as the induction base — if that segment is
  // walked, every taken-address in it must stay raw.
  uint64_t SegLo = 0, SegHi = 0;
  if (Img)
    if (const Segment *Seg = Img->getSegmentFor(Val)) {
      SegLo = Seg->VA;
      SegHi = Seg->VA + std::max<uint64_t>(Seg->Data.size(), 1);
    }
  auto inValSeg = [&](uint64_t C) {
    return SegHi != 0 && C >= SegLo && C < SegHi;
  };
  auto findDef = [&](const MedVar &Y) { return lookupDef(Y); };
  auto findPhi = [&](const MedVar &Y) { return lookupPhi(Y); };
  // Threads the copy/width/add/sub/select/phi chain a walked pointer carries
  // its init and wrap-around reset through, looking for either the constant
  // base
  // \p Val (\p WantConst) or the PHI output \p Target (advance detection).
  auto walk = [&](const MedVar &Start, bool WantConst, const MedVar &Target) {
    std::vector<MedVar> Work{Start};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 512;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        if (WantConst && inValSeg(Cur.ConstVal))
          return true;
        continue;
      }
      if (WantConst)
        if (auto C = traceSSAConst(Cur))
          if (inValSeg(*C))
            return true;
      if (!WantConst && Cur.Kind == Target.Kind && Cur.Id == Target.Id &&
          Cur.SSAVer == Target.SSAVer)
        return true;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Ph = findPhi(Cur)) {
        for (const auto &[Pred, A] : Ph->Args) {
          if (!phiIncomingEdgeFeasible(*Ph, Pred))
            continue;
          Work.push_back(A);
        }
        continue;
      }
      if (const MedOp *D = findDef(Cur)) {
        if (auto Forwarded = pointerPreservingInput(*D)) {
          Work.push_back(*Forwarded);
          continue;
        }
        switch (D->Opcode) {
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
          for (int I = 0; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        case NdOp::SELECT:
          if (!selectPreservesPointerValues(*D))
            break;
          Work.push_back(D->Inputs[1]);
          Work.push_back(D->Inputs[2]);
          break;
        default:
          break;
        }
      }
    }
    return false;
  };
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &P : Blk.Phis) {
      bool HasBase = false, HasAdvance = false;
      for (const auto &[Pred, Arg] : P.Args) {
        if (!phiIncomingEdgeFeasible(P, Pred))
          continue;
        if (walk(Arg, /*WantConst=*/true, P.Output))
          HasBase = true;
        // A self-advancing back-edge (`p = ... p ± stride ...`, possibly behind
        // a wrap-around SELECT) re-derives the PHI output through an arithmetic
        // op.
        if (!Arg.isConst() && walk(Arg, /*WantConst=*/false, P.Output)) {
          MedVar A2 = Arg;
          const MedOp *D = findDef(A2);
          // i386 PIC re-zeroes the 32-bit advance into the 64-bit pointer PHI
          // as `zext(p32 ± stride)`; peel COPY/ZEXT/SEXT/low-SUBBYTES so the
          // wrapped ADD is still classified as an advance (else pdstore/mat2d's
          // reloaded search pointer is missed and its base is wrongly
          // symbolized).  Scoped to i386 so the AArch64/ARM induction detection
          // is unchanged (a64 ptrcmp's run-end boundary must stay
          // symbolizable).
          if (TargetArch == Arch::X86)
            while (D && D->NumInputs >= 1 &&
                   (D->Opcode == NdOp::COPY || D->Opcode == NdOp::INT_ZEXT ||
                    D->Opcode == NdOp::INT_SEXT ||
                    (D->Opcode == NdOp::SUBBYTES && D->NumInputs >= 2 &&
                     D->Inputs[1].isConst() && D->Inputs[1].ConstVal == 0))) {
              A2 = D->Inputs[0];
              D = findDef(A2);
            }
          if (D && (D->Opcode == NdOp::INT_ADD || D->Opcode == NdOp::INT_SUB ||
                    D->Opcode == NdOp::SELECT))
            HasAdvance = true;
        }
      }
      if (HasBase && HasAdvance)
        return true;
    }
  return false;
}

bool MedLLVMEmitter::isCleanRodataStringAddress(uint64_t Val) const {
  if (!Img || Val == 0)
    return false;
  const Segment *Seg = Img->getSegmentFor(Val);
  if (!Seg || Seg->isWritable() || Img->isCodeAddress(Val) ||
      Seg->Data.empty() || segHasPtrRelocSlots(Seg) || Val < Seg->VA)
    return false;
  size_t Off = static_cast<size_t>(Val - Seg->VA);
  if (Off >= Seg->Data.size())
    return false;
  const uint8_t *S = Seg->Data.data() + Off;
  size_t Max = Seg->Data.size() - Off, Len = 0;
  for (size_t I = 0; I < Max && I < limits::kMaxStringScanLen; ++I) {
    if (S[I] == 0)
      return Len >= 2;
    uint8_t Ch = S[I];
    if ((Ch >= 0x20 && Ch < 0x7F) || Ch == '\n' || Ch == '\r' || Ch == '\t' ||
        Ch >= 0x80)
      ++Len;
    else
      return false;
  }
  return false;
}

bool MedLLVMEmitter::isInductionRodataStringBase(uint64_t Val) const {
  if (!CurMedFunc || !Img || Val == 0)
    return false;
  if (InductionBasesFor != CurMedFunc) {
    InductionBasesFor = CurMedFunc;
    InductionBaseVAs.clear();

    auto findPhi = [&](const MedVar &V) { return lookupPhi(V); };
    auto findDef = [&](const MedVar &V) { return lookupDef(V); };

    // True when getVar's existing constant-pointer condition would symbolize
    // this base to a recompiled-VA pointer — the i386/ARM32 PIC reloc/anchor
    // target, or a VA above the global-data threshold.  Only when a walked base
    // is symbolized this way does the pointer become MIXED (this arm a
    // recompiled VA, a sibling `base+k` arm a bare original VA), which is the
    // divergence we unify.  x86-64/AArch64 keep every arm a bare origVA
    // constant (no reloc), so none is flagged and they stay on the @run
    // induction model below.
    auto wouldExistingSymbolize = [&](uint64_t VA) {
      return getVarDirectlySymbolizesDataConstant(
          VA, getTargetRegInfo(TargetArch).PointerSize);
    };

    // The induction base of a PHI arg: a rodata constant reached as a direct
    // const-base init, a literal-pool base, or a `base + index` (i386 GOTOFF
    // `RAX_GOT + W@GOTOFF`).  Mirrors tryResolveInductionGlobalPtr's base scan.
    auto argBase = [&](const MedVar &Arg) -> std::optional<uint64_t> {
      if (varIsFrameDerived(Arg))
        return std::nullopt;
      if (auto C = traceTableBaseConst(Arg, 0, nullptr))
        return C;
      uint64_t B = 0;
      bool Have = false;
      std::vector<MedVar> Idx;
      if (collectLiteralPoolBase(Arg, B, Have, Idx) && Have)
        return B;
      B = 0;
      Have = false;
      Idx.clear();
      if (collectIndexedGlobalBase(Arg, B, Have, Idx) && Have)
        return B;
      return std::nullopt;
    };

    // Walk each memory-address operand back through the
    // COPY/ZEXT/SEXT/SUBBYTES/ ADD/SUB/SELECT/PHI chain
    // tryResolveInductionGlobalPtr follows.  Only when the address reaches a
    // PHI (an induction pointer, not a plain `base + index` direct access)
    // record every clean-rodata-string base the walk found — a PHI-arg base
    // (collectIndexedGlobalBase: i386 GOTOFF `SUBBYTES(GOT + W@off)`) and a
    // bare rodata-VA constant materialized inline (the advance arm's `base + k`
    // / the wrap-around `&W` SELECT arm).  Restricting to strings keeps rodata
    // *table* induction walks on their existing path.
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Op.NumInputs < 1)
          continue;
        std::vector<MedVar> Work{Op.Inputs[0]};
        std::set<std::tuple<int, int, int>> Seen;
        std::set<uint64_t> Cand;
        bool SawPhi = false, SawSymbolized = false;
        int Budget = 256;
        auto note = [&](uint64_t B) {
          if (!isCleanRodataStringAddress(B))
            return;
          Cand.insert(B);
          if (wouldExistingSymbolize(B))
            SawSymbolized = true;
        };
        while (!Work.empty() && Budget-- > 0) {
          MedVar Cur = Work.back();
          Work.pop_back();
          if (Cur.isConst()) {
            note(Cur.ConstVal);
            continue;
          }
          if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
            continue;
          if (const PhiNode *P = findPhi(Cur)) {
            SawPhi = true;
            for (const auto &[Pred, Arg] : P->Args) {
              if (!phiIncomingEdgeFeasible(*P, Pred))
                continue;
              if (auto B = argBase(Arg))
                note(*B);
              Work.push_back(Arg);
            }
            continue;
          }
          if (const MedOp *D = findDef(Cur)) {
            if (auto Forwarded = pointerPreservingInput(*D))
              Work.push_back(*Forwarded);
            else if ((D->Opcode == NdOp::INT_ADD ||
                      D->Opcode == NdOp::INT_SUB) &&
                     D->NumInputs >= 2) {
              Work.push_back(D->Inputs[0]);
              Work.push_back(D->Inputs[1]);
            } else if (D->Opcode == NdOp::SELECT &&
                       selectPreservesPointerValues(*D)) {
              Work.push_back(D->Inputs[1]);
              Work.push_back(D->Inputs[2]);
            }
          }
        }
        // Flag only a MIXED-model walk: reached a PHI (a walked/reset pointer)
        // AND at least one base getVar already symbolizes (the i386 PIC GOTOFF
        // reloc base).  A uniform-origVA walk (x86-64/AArch64) has no
        // symbolized base, so it is left on the @run induction model.
        if (SawPhi && SawSymbolized)
          InductionBaseVAs.insert(Cand.begin(), Cand.end());
      }
  }
  return InductionBaseVAs.count(Val) != 0;
}

} // namespace neverd
