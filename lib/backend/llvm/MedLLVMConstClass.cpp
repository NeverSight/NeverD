//===- MedLLVMConstClass.cpp - Constant pointer/integer classification -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-function constant classification for MedLLVMEmitter: deciding whether a
/// bare integer constant is used as a pointer or as an integer, recognising the
/// rodata / writable run-end bounds, pointer-difference and induction bases, and
/// the writable-reloc / select-peer symbolization predicates, plus the small
/// caches that keep those queries cheap.  These answer the "is this constant an
/// address?" questions that getVar consults in MedLLVMVarAccess.cpp; the
/// variable materialization itself (getVar/setVar) and the def/phi index those
/// classifiers walk live there.  Every routine here is a MedLLVMEmitter member
/// declared in the shared header, so this is a pure translation-unit split.
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
    if (!isMutableDataSeg(&S))
      continue;
    uint64_t End = S.VA + (S.Size ? S.Size : S.Data.size());
    if (Val == End) {
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
        case NdOp::SELECT:
          for (int I = 0; I < D->NumInputs; ++I)
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

bool MedLLVMEmitter::symbolizesWritableRelocPtr(uint64_t Val,
                                                uint16_t Size) const {
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
        case NdOp::SELECT:
          for (int I = 0; I < D->NumInputs; ++I)
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
        case NdOp::SELECT:
          for (int I = 0; I < D->NumInputs; ++I)
            Work.push_back(D->Inputs[I]);
          break;
        default:
          break;
        }
    }
    return false;
  };
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &P : Blk.Phis) {
      bool HasBase = false, HasAdvance = false;
      for (const auto &[Pred, Arg] : P.Args) {
        (void)Pred;
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
      if (constUsedAsPointer(C) && !constValueUsedAsInteger(C) &&
          !addrInCodePtrMirrorRun(C))
        SymbolizedWritableSegs.insert(Seg->VA);
    }
  }
  const Segment *VSeg = Img->getSegmentFor(Val);
  return VSeg && SymbolizedWritableSegs.count(VSeg->VA);
}

bool MedLLVMEmitter::symbolizesSelectPeer(uint64_t Val) const {
  if (!Img || !CurMedFunc || Val == 0)
    return false;
  const Segment *VSeg = Img->getSegmentFor(Val);
  if (!VSeg || !VSeg->isWritable())
    return false;
  // Same walked-base protection the reloc-target fallback uses: never pull a
  // pointer-difference base or a (segment-) walked induction base onto the
  // recompiled-pointer model.  No constUsedAsPointer gate is needed — being the
  // SELECT peer of a proven recompiled pointer (checked below) is itself the
  // pointer evidence, and constUsedAsPointer's backward walk does not even
  // traverse SELECT, so it would spuriously reject every genuine peer.
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
        case NdOp::SELECT:
          for (int I = 0; I < D->NumInputs; ++I)
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
      if (Op.Opcode != NdOp::SELECT || Op.NumInputs < 2)
        continue;
      for (int I = 0; I < Op.NumInputs; ++I)
        for (int J = 0; J < Op.NumInputs; ++J) {
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
        case NdOp::SELECT:
        case NdOp::INT_AND:
        case NdOp::INT_OR:
        case NdOp::INT_XOR:
          for (int I = 0; I < D->NumInputs; ++I)
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


void MedLLVMEmitter::ensureConstClassCache() const {
  if (ConstClassCacheFor == CurMedFunc)
    return;
  ConstClassCacheFor = CurMedFunc;
  ConstUsedAsPointerCache.clear();
  ConstValueUsedAsIntegerCache.clear();
}

bool MedLLVMEmitter::constUsedAsPointer(uint64_t Val) const {
  ensureConstClassCache();
  auto It = ConstUsedAsPointerCache.find(Val);
  if (It != ConstUsedAsPointerCache.end())
    return It->second;
  bool Result = constUsedAsPointerImpl(Val);
  ConstUsedAsPointerCache.emplace(Val, Result);
  return Result;
}

bool MedLLVMEmitter::constUsedAsPointerImpl(uint64_t Val) const {
  if (!CurMedFunc)
    return false;

  // Backward walk from every memory-access address through address arithmetic;
  // return true when any visited operand satisfies \p Match.
  auto reaches = [&](auto &&Match) {
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.NumInputs < 1)
          continue;
        std::vector<MedVar> Work{Op.Inputs[0]};
        std::set<std::tuple<int, int, int>> Seen;
        int Budget = 256;
        while (!Work.empty() && Budget-- > 0) {
          MedVar Cur = Work.back();
          Work.pop_back();
          if (Match(Cur))
            return true;
          if (Cur.isConst())
            continue;
          if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
            continue;
          if (const MedOp *D = lookupDef(Cur))
            switch (D->Opcode) {
            // Additive / compositional address forming: a constant operand can
            // be a base (`baseConst + index`, or `alignedBase | scaledIndex`),
            // so follow every operand.
            case NdOp::INT_ADD:
            case NdOp::INT_SUB:
            case NdOp::INT_OR:
            case NdOp::INT_XOR:
            case NdOp::INT_ZEXT:
            case NdOp::INT_SEXT:
            case NdOp::COPY:
            case NdOp::SUBBYTES:
              for (int I = 0; I < D->NumInputs; ++I)
                Work.push_back(D->Inputs[I]);
              break;
            // Index arithmetic: a CONSTANT operand is a mask (AND), scale
            // (MULT), or shift amount (LEFT) — never a base address.  A
            // scaled-index byte mask `(w >> 9) & ((2^11-1)<<2)` carries 0x1FFC,
            // which can land inside a low-VA `.bss` run's range; it must stay
            // an integer, not be taken for a pointer into that run.  Follow
            // only the non-constant operand (the value being masked/scaled,
            // which may trace back to a real base).
            case NdOp::INT_AND:
            case NdOp::INT_MULT:
            case NdOp::INT_LEFT:
              for (int I = 0; I < D->NumInputs; ++I)
                if (!D->Inputs[I].isConst())
                  Work.push_back(D->Inputs[I]);
              break;
            default:
              break;
            }
        }
      }
    return false;
  };

  // (1) The constant (or anything derived from it) is a memory-access address.
  if (reaches(
          [&](const MedVar &C) { return C.isConst() && C.ConstVal == Val; }))
    return true;

  // (2) The constant is compared / offset against a value that is itself a
  //     pointer (`p != end`, `end - begin`): the one-past-the-end idioms.
  auto varIsPointer = [&](const MedVar &Y) {
    if (Y.isConst())
      return false;
    return reaches([&](const MedVar &C) {
      return !C.isConst() && C.Kind == Y.Kind && C.Id == Y.Id &&
             C.SSAVer == Y.SSAVer;
    });
  };
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      switch (Op.Opcode) {
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
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
      for (int I = 0; I < 2; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == Val &&
            varIsPointer(Op.Inputs[1 - I]))
          return true;
    }
  return false;
}

bool MedLLVMEmitter::constValueUsedAsInteger(uint64_t Val) const {
  ensureConstClassCache();
  auto It = ConstValueUsedAsIntegerCache.find(Val);
  if (It != ConstValueUsedAsIntegerCache.end())
    return It->second;
  bool Result = constValueUsedAsIntegerImpl(Val);
  ConstValueUsedAsIntegerCache.emplace(Val, Result);
  return Result;
}

bool MedLLVMEmitter::constValueUsedAsIntegerImpl(uint64_t Val) const {
  if (!CurMedFunc)
    return false;

  // Does var X (or anything derived from it through address arithmetic) serve
  // as a LOAD/STORE address operand?  A backward walk from every memory access
  // address; if it reaches X, X is a pointer, not an integer.
  auto reachesMemAddr = [&](const MedVar &X) {
    auto sameVar = [&](const MedVar &A, const MedVar &B) {
      return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
             A.SSAVer == B.SSAVer;
    };
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.NumInputs < 1)
          continue;
        std::vector<MedVar> Work{Op.Inputs[0]};
        std::set<std::tuple<int, int, int>> Seen;
        int Budget = 256;
        while (!Work.empty() && Budget-- > 0) {
          MedVar Cur = Work.back();
          Work.pop_back();
          if (Cur.isConst())
            continue;
          if (sameVar(Cur, X))
            return true;
          if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
            continue;
          if (const MedOp *D = lookupDef(Cur))
            switch (D->Opcode) {
            case NdOp::INT_ADD:
            case NdOp::INT_SUB:
            case NdOp::INT_AND:
            case NdOp::INT_OR:
            case NdOp::INT_XOR:
            case NdOp::INT_LEFT:
            case NdOp::INT_MULT:
            case NdOp::INT_ZEXT:
            case NdOp::INT_SEXT:
            case NdOp::COPY:
            case NdOp::SUBBYTES:
              for (int I = 0; I < D->NumInputs; ++I)
                Work.push_back(D->Inputs[I]);
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
      bool HasConst = false;
      std::set<uint64_t> DistinctConsts;
      for (const auto &[Pred, Arg] : P.Args) {
        (void)Pred;
        // The init value may be an inline const or a COPY of one (`COPY ESI,
        // 0xA0` feeding the counter PHI), so fold through copies.
        auto C = Arg.isConst() ? std::optional<uint64_t>(Arg.ConstVal)
                               : traceSSAConst(Arg);
        if (C) {
          DistinctConsts.insert(*C);
          if (*C == Val)
            HasConst = true;
        }
      }
      // A loop counter PHI has exactly ONE constant arg (the init) plus a
      // computed back-edge increment, so its const is a genuine integer.  A
      // multi-way SELECTION PHI (a `switch` returning string literals merges
      // the several `&"..."` arms, `cond ? &A : &B`) has SEVERAL constant args
      // that are pointers — the returned address is dereferenced in the CALLER,
      // so reachesMemAddr is locally false and must NOT mark these reloc-target
      // pointer constants as integers.  Only the single-const counter form
      // does.
      if (HasConst && DistinctConsts.size() == 1 && !reachesMemAddr(P.Output))
        return true;
    }

  // A constant consumed as an INT_MULT factor is a multiplier/scale, never a
  // pointer (pointers are not multiplied).  When the product never flows into a
  // memory address it is a pure integer computation (e.g. the hash multiplier
  // `h*131`), so a value that merely equals a rodata reloc-target VA (the i386
  // switch-string table places a 5-char string at VA 0x83==131) must stay an
  // integer rather than be redirected to that string global.  Gated on the
  // product not reaching a load/store address so an `index*scale` that forms an
  // address is unaffected.
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::INT_MULT)
        continue;
      bool HasConst = false;
      for (int I = 0; I < Op.NumInputs; ++I) {
        auto C = Op.Inputs[I].isConst()
                     ? std::optional<uint64_t>(Op.Inputs[I].ConstVal)
                     : traceSSAConst(Op.Inputs[I]);
        if (C && *C == Val) {
          HasConst = true;
          break;
        }
      }
      if (HasConst && !reachesMemAddr(Op.Output))
        return true;
    }

  // A constant compared (==/!=/</<=, signed or unsigned) against a value that
  // is itself a pure integer — a loop-trip-count bound tested against the
  // induction counter (`i+1 == N`) — is an integer, not a pointer.  Without
  // this a bound whose value merely equals a low rodata/string reloc-target VA
  // (the i386
  // `.o` places "hotel" at VA 0xC8 == loop bound 200) is redirected to that
  // string global and the loop count is destroyed.  Guarded so a genuine
  // pointer comparison `p == &g` (where p reaches a memory address, i.e. is a
  // pointer) keeps &g redirected.  Only reachable for a low-VA reloc-target
  // constant: the `> kMinGlobalDataAddr` redirect path short-circuits earlier.
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
        if (!C || *C != Val)
          continue;
        const MedVar &Other = Op.Inputs[1 - I];
        // The compared-against value is a pure integer (not a dereferenced
        // pointer): no constant pointer comparison, and it never feeds a memory
        // address.  That makes Val an integer bound.
        if (!Other.isConst() && !reachesMemAddr(Other))
          return true;
      }
    }
  return false;
}

bool MedLLVMEmitter::isInductionRodataStringBase(uint64_t Val) {
  if (!CurMedFunc || !Img || Val == 0)
    return false;
  if (InductionBasesFor != CurMedFunc) {
    InductionBasesFor = CurMedFunc;
    InductionBaseVAs.clear();

    auto findPhi = [&](const MedVar &V) { return lookupPhi(V); };
    auto findDef = [&](const MedVar &V) { return lookupDef(V); };

    // A bare VA that begins a C-string in a plain read-only segment (no
    // relocated pointer slots — those belong to the data-pointer-table path the
    // DataPtrRelocSlots bail in the induction resolver handles).
    auto isCleanRodataString = [&](uint64_t C) -> bool {
      const Segment *Seg = Img->getSegmentFor(C);
      if (!Seg || Seg->isWritable() || Seg->isExecutable() || Seg->Data.empty())
        return false;
      if (segHasPtrRelocSlots(Seg) || C < Seg->VA)
        return false;
      size_t Off = static_cast<size_t>(C - Seg->VA);
      if (Off >= Seg->Data.size())
        return false;
      const uint8_t *S = Seg->Data.data() + Off;
      size_t Max = Seg->Data.size() - Off, Len = 0;
      for (size_t I = 0; I < Max && I < limits::kMaxStringScanLen; ++I) {
        if (S[I] == 0)
          return Len >= 2;
        uint8_t Ch = S[I];
        if ((Ch >= 0x20 && Ch < 0x7F) || Ch == '\n' || Ch == '\r' ||
            Ch == '\t' || Ch >= 0x80)
          Len++;
        else
          return false;
      }
      return false;
    };

    // True when getVar's existing constant-pointer condition would symbolize
    // this base to a recompiled-VA pointer — the i386/ARM32 PIC reloc/anchor
    // target, or a VA above the global-data threshold.  Only when a walked base
    // is symbolized this way does the pointer become MIXED (this arm a
    // recompiled VA, a sibling `base+k` arm a bare original VA), which is the
    // divergence we unify.  x86-64/AArch64 keep every arm a bare origVA
    // constant (no reloc), so none is flagged and they stay on the @run
    // induction model below.
    auto wouldExistingSymbolize = [&](uint64_t VA) {
      return VA > limits::kMinGlobalDataAddr ||
             ((Img->RelocDataAddrs.count(VA) ||
               Img->RodataAnchorSeg.count(VA)) &&
              !constValueUsedAsInteger(VA));
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
            Op.NumInputs < 1)
          continue;
        std::vector<MedVar> Work{Op.Inputs[0]};
        std::set<std::tuple<int, int, int>> Seen;
        std::set<uint64_t> Cand;
        bool SawPhi = false, SawSymbolized = false;
        int Budget = 256;
        auto note = [&](uint64_t B) {
          if (!isCleanRodataString(B))
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
              (void)Pred;
              if (auto B = argBase(Arg))
                note(*B);
              Work.push_back(Arg);
            }
            continue;
          }
          if (const MedOp *D = findDef(Cur)) {
            if ((D->Opcode == NdOp::COPY || D->Opcode == NdOp::INT_ZEXT ||
                 D->Opcode == NdOp::INT_SEXT || D->Opcode == NdOp::SUBBYTES) &&
                D->NumInputs >= 1)
              Work.push_back(D->Inputs[0]);
            else if ((D->Opcode == NdOp::INT_ADD ||
                      D->Opcode == NdOp::INT_SUB) &&
                     D->NumInputs >= 2) {
              Work.push_back(D->Inputs[0]);
              Work.push_back(D->Inputs[1]);
            } else if (D->Opcode == NdOp::SELECT && D->NumInputs >= 3) {
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
