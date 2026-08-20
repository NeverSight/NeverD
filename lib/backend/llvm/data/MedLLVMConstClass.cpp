//===- MedLLVMConstClass.cpp - Constant address classification -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Run-boundary and symbolization classification of a bare constant for
/// MedLLVMEmitter: recognising the one-past-the-end bound of a rodata or
/// writable run, and deciding when a proven relocation target (or its
/// select peer / segment sibling) is symbolized to a relocatable
/// recompiled pointer.  The pointer-vs-integer use classifiers live in
/// MedLLVMConstUse.cpp and the walked-base classifiers in
/// MedLLVMConstWalkedBase.cpp.  These answer the "is this constant an
/// address?" questions that getVar consults in
/// resolve/MedLLVMVarAccess.cpp.  Every routine here is a MedLLVMEmitter
/// member declared in the shared header, so this is a pure
/// translation-unit split.
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

//===----------------------------------------------------------------------===//
// Constant pointer / integer classification
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::constIsRodataEndPointer(uint64_t Val) const {
  if (!Img || Val == 0 || Img->getSegmentFor(Val))
    return false;
  bool AtEnd = false;
  for (const auto &S : Img->Segments) {
    if (S.isExecutable() || S.isWritable() || S.Data.empty())
      continue;
    if (Val == S.VA + S.Data.size()) {
      AtEnd = true;
      break;
    }
  }
  if (!AtEnd)
    return false;
  // A one-past-the-end VA can collide with a stack-frame displacement (`SP +
  // disp` where disp happens to equal a rodata run's end VA — common with large
  // SIMD spill frames).  Such a constant is a frame offset, not a table bound,
  // so never redirect it: exclude any constant added to / subtracted from a
  // frame-derived value.
  if (CurMedFunc)
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
            Op.NumInputs < 2)
          continue;
        for (int I = 0; I < 2; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == Val &&
              varIsFrameDerived(Op.Inputs[1 - I]))
            return false;
      }
  // The VA equals a segment end only by coincidence unless it is actually used
  // as a pointer.  A SIMD lane immediate that happens to equal `&tab[N]` (e.g.
  // a NEON `mov v.b[i], #0x88` where 0x88 is a rodata run's end) is assembled
  // into a vector value, never dereferenced — keep it an integer.
  if (!constUsedAsPointer(Val))
    return false;
  return true;
}

bool MedLLVMEmitter::constIsWritableRunEndPointer(uint64_t Val) const {
  // Must be a one-past-the-end value: not inside any segment.
  if (!Img || !CurMedFunc || Val == 0 || Img->getSegmentFor(Val))
    return false;
  const Segment *EndSeg = nullptr;
  for (const auto &S : Img->Segments) {
    uint64_t RunStart = 0, RunEnd = 0;
    if (writableRunBounds(S.VA, RunStart, RunEnd) && Val == RunEnd) {
      EndSeg = &S;
      break;
    }
  }
  if (!EndSeg)
    return false;
  // Only when the segment body is already on the recompiled-pointer model (some
  // taken-address in it is symbolized) so the bound joins the same model as the
  // walked pointer; a raw-rebased walk keeps its bound raw.
  if (!hasSymbolizedWritableSibling(EndSeg->VA))
    return false;
  const uint64_t SegLo = EndSeg->VA, SegHi = Val;

  auto findDef = [&](const MedVar &Y) { return lookupDef(Y); };
  auto findPhi = [&](const MedVar &Y) { return lookupPhi(Y); };
  // True when \p Start traces — through copy/width/add/sub/select AND the
  // induction PHI a walked pointer carries its base through — to a base
  // constant inside EndSeg.  Traversing the PHI is what makes this robust where
  // constUsedAsPointer / constValueUsedAsInteger miss the bound (their backward
  // walks stop at the loop PHI, so a `walked_ptr < &G[N]` comparison neither
  // proves the bound a pointer nor avoids flagging it an integer).
  auto reachesSegBase = [&](const MedVar &Start) {
    std::vector<MedVar> Work{Start};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 512;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      auto C = Cur.isConst() ? std::optional<uint64_t>(Cur.ConstVal)
                             : traceSSAConst(Cur);
      // traceSSAConst only follows COPY, so it cannot fold an i386 PIC base
      // reference (`base0 + GOTOFF`, base0 itself a 32-bit-wrapping const
      // chain) to the segment base it carries — without which a `walked_ptr <
      // &G[N]` bound is never proven a pointer.  Fall back to the width-aware
      // folder.
      if (!C)
        C = traceValueVA(Cur);
      if (C && *C >= SegLo && *C < SegHi)
        return true;
      if (Cur.isConst())
        continue;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Ph = findPhi(Cur)) {
        for (const auto &[Pred, A] : Ph->Args) {
          (void)Pred;
          Work.push_back(A);
        }
        continue;
      }
      if (const MedOp *D = findDef(Cur))
        switch (D->Opcode) {
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
        case NdOp::SUBBYTES:
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
          for (int I = 0; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        case NdOp::SELECT:
          for (int I = 1; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        default:
          break;
        }
    }
    return false;
  };
  // Val is a genuine walk BOUND only when it is compared against a value that
  // walks EndSeg (a same-segment base pointer) — `walked_ptr < &G[N]`.  An
  // integer that merely equals a segment end but is never compared against a
  // pointer into that segment stays raw (the #456 collision family).
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      switch (Op.Opcode) {
      case NdOp::INT_EQUAL:
      case NdOp::INT_NOTEQUAL:
      case NdOp::INT_LESS:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESSEQUAL:
      case NdOp::INT_SLESSEQUAL:
        break;
      default:
        continue;
      }
      if (Op.NumInputs < 2)
        continue;
      for (int I = 0; I < 2; ++I) {
        auto C = Op.Inputs[I].isConst()
                     ? std::optional<uint64_t>(Op.Inputs[I].ConstVal)
                     : traceSSAConst(Op.Inputs[I]);
        // The bound operand is an i386 PIC reference `base0 + GOTOFF` whose
        // base0 traceSSAConst cannot fold; recover the absolute VA with the
        // width-aware folder so `walked_ptr < &G[N]` is recognised.
        if (!C)
          C = traceValueVA(Op.Inputs[I]);
        if (C && *C == Val && reachesSegBase(Op.Inputs[1 - I]))
          return true;
      }
    }
  return false;
}

bool MedLLVMEmitter::writableRelocPtrHasSymbolizationIntent(
    uint64_t Val, uint16_t Size) const {
  if (!Img || !Img->WritableRelocDataAddrs.count(Val))
    return false;
  unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
  if (!(Size == 0 || PtrSz == 0 || Size >= PtrSz))
    return false;
  if (addrInCodePtrMirrorRun(Val))
    return false;
  // A walked base — feeding a pointer DIFFERENCE (`q - p`) or a self-advancing
  // induction (`p = &G; *(p ± k)`) — must keep its original VA so the rebase /
  // difference stays in one model.  The reloaded i386 search pointer reaches
  // here with usedInt=0 and would be symbolized by the early return below,
  // which the search-loop deref's GEP rebase then double-relocates (pdstore /
  // mat2d). Apply the walked guard before the early symbolize.  Scoped to i386:
  // the induction detection enhancements (above) are i386-only and AArch64/ARM
  // walked bases already round-trip via their existing paths (this guard must
  // not keep an a64 run-end boundary raw — #482 ptrcmp).
  if (TargetArch == Arch::X86 &&
      (valIsPointerDiffBase(Val) || valIsAdvancingInductionBase(Val)))
    return false;
  if (!constValueUsedAsInteger(Val))
    return true;
  // The integer heuristic flagged Val, but the loader PROVED
  // (WritableRelocDataAddrs above) it is a taken address — an integer constant
  // is never a data-relocation target, so the flag is a false positive (its low
  // VA was hoisted into an invariant self-PHI / consumed by an AND-mask blend
  // the counter/mask heuristics misread).  Symbolize it when it is used as a
  // pointer here AND a SIBLING taken- address in the same writable segment is
  // already on the recompiled-pointer model, so a `cond ? &A : &B` pointer
  // select does not leave one arm a stale raw VA.  EXCLUDE a walked base — one
  // feeding a pointer DIFFERENCE (`q - p`, pdtwo) or a self-advancing induction
  // (`p = &G; *(p ± k)`) — which must keep its original VA so the difference /
  // embedded-run rebase stays in one model.
  return constUsedAsPointer(Val) && hasSymbolizedWritableSibling(Val) &&
         !valIsPointerDiffBase(Val) && !valIsAdvancingInductionBase(Val);
}

bool MedLLVMEmitter::symbolizesWritableRelocPtr(uint64_t Val,
                                                uint16_t Size) const {
  return writableRelocPtrHasSymbolizationIntent(Val, Size) &&
         canResolveGlobalDataConstant(Val);
}

bool MedLLVMEmitter::hasSymbolizedWritableSibling(uint64_t Val) const {
  if (!Img || !CurMedFunc)
    return false;
  if (SymbolizedWritableSegsFor != CurMedFunc) {
    SymbolizedWritableSegsFor = CurMedFunc;
    SymbolizedWritableSegs.clear();
    // Gather every distinct constant the function materializes, then record the
    // writable segment of each one the loader proved is a relocation target and
    // that getVar already symbolizes (used as a pointer, not flagged integer).
    std::set<uint64_t> Consts;
    auto note = [&](const MedVar &V) {
      if (V.isConst())
        Consts.insert(V.ConstVal);
    };
    for (const auto &Blk : CurMedFunc->Blocks) {
      for (const auto &Op : Blk.Ops)
        for (int I = 0; I < Op.NumInputs; ++I)
          note(Op.Inputs[I]);
      for (const auto &P : Blk.Phis)
        for (const auto &[Pred, Arg] : P.Args) {
          (void)Pred;
          note(Arg);
        }
    }
    for (uint64_t C : Consts) {
      if (!Img->WritableRelocDataAddrs.count(C))
        continue;
      const Segment *Seg = Img->getSegmentFor(C);
      if (!Seg)
        continue;
      // A sibling that the existing symbolization predicate already accepts:
      // used as a pointer and NOT flagged as an integer.
      if (canResolveGlobalDataConstant(C) && constUsedAsPointer(C) &&
          !constValueUsedAsInteger(C) && !addrInCodePtrMirrorRun(C))
        SymbolizedWritableSegs.insert(Seg->VA);
    }
  }
  const Segment *VSeg = Img->getSegmentFor(Val);
  return VSeg && SymbolizedWritableSegs.count(VSeg->VA);
}

bool MedLLVMEmitter::mayBeWritableSelectPeerCycleFree(uint64_t Val) const {
  if (!Img || !CurMedFunc || Val == 0)
    return false;
  const Segment *VSeg = Img->getSegmentFor(Val);
  if (!VSeg || !VSeg->isReadable() || !VSeg->isWritable() ||
      !canResolveGlobalDataConstant(Val))
    return false;

  auto reaches = [&](const MedVar &Start, auto &&Pred, bool AllowBitwise) {
    std::vector<MedVar> Work{Start};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        if (Pred(Cur.ConstVal))
          return true;
        continue;
      }
      if (auto C = traceSSAConst(Cur); C && Pred(*C))
        return true;
      if (!Seen.insert({static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Phi = lookupPhi(Cur)) {
        for (const auto &[PredId, Arg] : Phi->Args) {
          (void)PredId;
          Work.push_back(Arg);
        }
        continue;
      }
      const MedOp *Def = lookupDef(Cur);
      if (!Def)
        continue;
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::SUBBYTES:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
        for (int I = 0; I < Def->NumInputs; ++I)
          Work.push_back(Def->Inputs[I]);
        break;
      case NdOp::SELECT:
        // A SELECT condition is a scalar decision, never an address arm.
        for (int I = 1; I < Def->NumInputs; ++I)
          Work.push_back(Def->Inputs[I]);
        break;
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
        if (AllowBitwise)
          for (int I = 0; I < Def->NumInputs; ++I)
            Work.push_back(Def->Inputs[I]);
        break;
      default:
        break;
      }
    }
    return false;
  };

  auto IsVal = [&](uint64_t C) { return C == Val; };
  auto IsLoaderProvenPeer = [&](uint64_t C) {
    if (C == Val || !Img->WritableRelocDataAddrs.count(C) ||
        !canResolveGlobalDataConstant(C))
      return false;
    const Segment *CSeg = Img->getSegmentFor(C);
    return CSeg && CSeg->VA == VSeg->VA;
  };

  for (const MedBlock &Block : CurMedFunc->Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::SELECT && Op.NumInputs >= 3) {
        for (int I = 1; I < Op.NumInputs; ++I)
          for (int J = 1; J < Op.NumInputs; ++J)
            if (I != J && reaches(Op.Inputs[I], IsVal, false) &&
                reaches(Op.Inputs[J], IsLoaderProvenPeer, false))
              return true;
      }
      if (Op.Opcode == NdOp::INT_OR && Op.NumInputs >= 2)
        for (int I = 0; I < 2; ++I)
          for (int J = 0; J < 2; ++J)
            if (I != J && reaches(Op.Inputs[I], IsVal, true) &&
                reaches(Op.Inputs[J], IsLoaderProvenPeer, true))
              return true;
    }
  return false;
}

bool MedLLVMEmitter::symbolizesSelectPeer(uint64_t Val) const {
  if (!Img || !CurMedFunc || Val == 0)
    return false;
  const Segment *VSeg = Img->getSegmentFor(Val);
  if (!VSeg || !VSeg->isWritable() || !canResolveGlobalDataConstant(Val))
    return false;
  // Keep the control-flow upper bound and the precise classifier on one
  // structural domain. The exact checks below may reject candidates, but may
  // never accept a shape this cycle-free predicate omitted.
  if (!mayBeWritableSelectPeerCycleFree(Val))
    return false;
  // Same walked-base protection the reloc-target fallback uses: never pull a
  // pointer-difference base or a (segment-) walked induction base onto the
  // recompiled-pointer model.  No constUsedAsPointer gate is needed — being the
  // SELECT peer of a proven recompiled pointer (checked below) is itself the
  // pointer evidence. The generic constant-use walk intentionally follows
  // only SELECT value arms, while this classifier additionally audits the
  // peer arm's loader provenance.
  if (valIsPointerDiffBase(Val) || valIsAdvancingInductionBase(Val))
    return false;
  unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
  auto findDef = [&](const MedVar &Y) { return lookupDef(Y); };
  auto findPhi = [&](const MedVar &Y) { return lookupPhi(Y); };
  // True when \p Start reaches (through copy/width/add/sub/select/phi) a
  // constant that satisfies \p Pred.
  auto reaches = [&](const MedVar &Start, auto &&Pred) {
    std::vector<MedVar> Work{Start};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        if (Pred(Cur.ConstVal))
          return true;
        continue;
      }
      if (auto C = traceSSAConst(Cur))
        if (Pred(*C))
          return true;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Ph = findPhi(Cur)) {
        for (const auto &[Pred2, A] : Ph->Args) {
          (void)Pred2;
          Work.push_back(A);
        }
        continue;
      }
      if (const MedOp *D = findDef(Cur))
        switch (D->Opcode) {
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
        case NdOp::SUBBYTES:
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
          for (int I = 0; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        case NdOp::SELECT:
          for (int I = 1; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        default:
          break;
        }
    }
    return false;
  };
  auto isVal = [&](uint64_t C) { return C == Val; };
  auto isSymbolizedPeer = [&](uint64_t C) {
    if (C == Val)
      return false;
    const Segment *CSeg = Img->getSegmentFor(C);
    return CSeg && CSeg->VA == VSeg->VA &&
           symbolizesWritableRelocPtr(C, static_cast<uint16_t>(PtrSz));
  };
  // A SELECT whose one arm materializes Val and another arm a symbolized
  // same-segment pointer constant — `cond ? &A : &B` where &B already
  // relocates.
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::SELECT || Op.NumInputs < 3)
        continue;
      for (int I = 1; I < Op.NumInputs; ++I)
        for (int J = 1; J < Op.NumInputs; ++J) {
          if (I == J)
            continue;
          if (reaches(Op.Inputs[I], isVal) &&
              reaches(Op.Inputs[J], isSymbolizedPeer))
            return true;
        }
    }
  // clang also lowers a 2-way pointer selection WITHOUT a SELECT op, as a
  // branchless bitwise blend `(mask & P1) | (~mask & P2)` — the `~mask & P2`
  // arm materialized as `(mask & P2) ^ P2`.  Detect an INT_OR whose two
  // operands each derive (through the AND/XOR/OR mask chain plus the usual
  // add/sub/copy/width) from a DISTINCT constant: one is Val, the other a
  // symbolized same-segment pointer.  This is the SELECT-peer relationship
  // expressed in bitwise form (structval / structwr: the selected `&A`/`&C` are
  // carried by-value into a noinline callee, so neither arm is dereferenced
  // locally and the reloc gate's constUsedAsPointer alone never reaches them).
  // The same walked-base guards above protect it; the peer must still be a
  // proven symbolized pointer, so an integer that merely lands in an OR is not
  // pulled onto the pointer model.
  auto reachesBitwise = [&](const MedVar &Start, auto &&Pred) {
    std::vector<MedVar> Work{Start};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        if (Pred(Cur.ConstVal))
          return true;
        continue;
      }
      if (auto C = traceSSAConst(Cur))
        if (Pred(*C))
          return true;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Ph = findPhi(Cur)) {
        for (const auto &[Pred2, A] : Ph->Args) {
          (void)Pred2;
          Work.push_back(A);
        }
        continue;
      }
      if (const MedOp *D = findDef(Cur))
        switch (D->Opcode) {
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
        case NdOp::SUBBYTES:
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
        case NdOp::INT_AND:
        case NdOp::INT_OR:
        case NdOp::INT_XOR:
          for (int I = 0; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        case NdOp::SELECT:
          for (int I = 1; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        default:
          break;
        }
    }
    return false;
  };
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::INT_OR || Op.NumInputs < 2)
        continue;
      for (int I = 0; I < 2; ++I)
        for (int J = 0; J < 2; ++J) {
          if (I == J)
            continue;
          if (reachesBitwise(Op.Inputs[I], isVal) &&
              reachesBitwise(Op.Inputs[J], isSymbolizedPeer))
            return true;
        }
    }
  // NOTE: the cross-block PHI dual (`if(c) p=&A; else p=&B` merged by a PHI) is
  // intentionally NOT handled here.  clang -O2 lowers such two-way pointer
  // selections to a flat SELECT (covered above), while a genuine PHI in this
  // position is far more often a loop induction whose bound/counter coincides
  // with a large global's VA range (an 8 KiB `.bss` array's `[base, base+size)`
  // span swallows an integer trip count like 2044) — symbolizing that
  // coinciding integer corrupts the loop (the #456 family on a big global).  A
  // clean PHI path would need to prove the merged value is a pointer, not an
  // index.
  return false;
}

} // namespace neverd
