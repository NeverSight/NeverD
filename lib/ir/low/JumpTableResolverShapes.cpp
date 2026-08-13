//===- JumpTableResolverShapes.cpp - Composite table shapes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizers for composite and decoupled jump-table layouts:
/// runtime-selected two-table dispatch, constant-base absolute tables, and
/// two-level index tables.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

static uint64_t truncateToByteWidth(uint64_t Value, uint16_t Bytes) {
  if (Bytes == 0 || Bytes >= sizeof(Value))
    return Value;
  return Value & llvm::maskTrailingOnes<uint64_t>(Bytes * CHAR_BIT);
}

//===----------------------------------------------------------------------===//
// tryTwoTableSelect — runtime-selected table base (two adjacent tables)
//===----------------------------------------------------------------------===//

bool CFGBuilder::tryTwoTableSelect(const BinaryImage &Img,
                                   const InsnRecord &Rec, JumpTableInfo &Info) {
  if (!CurrentImg)
    return false;
  bool HasIndBranch = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  if (!HasIndBranch)
    return false;

  // Flatten the whole function prefix so a table base materialised in a
  // dominating block (the `lea`/`adrp`/`leal GOTOFF`) and a spill store of one
  // table base (i386 `cmov (%esp),...`) are both in scope.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);

  // Entry size of the table currently being matched; set per LOAD candidate in
  // the scan below.  foldArm uses it to prefer an arm fold that is an actual
  // code-pointer table over a stale non-table constant (see below).
  uint16_t TableEntW = 0;

  // Fold a select arm (one of the two candidate table-base sub-expressions) to
  // a constant table address.  A register arm is folded by emulating the prefix
  // up to the select (`Cutoff`), before the select overwrites it; a spilled arm
  // (i386 `cmov (%esp)`) is store-forwarded to the register that produced it.
  std::function<std::optional<uint64_t>(NdVar, int, va_t, int)> foldArm =
      [&](NdVar V, int From, va_t Cutoff,
          int Depth) -> std::optional<uint64_t> {
    if (Depth > limits::kMaxSliceDepth)
      return std::nullopt;
    if (V.isConst())
      return V.Offset;
    NdVar Cur = V;
    int CurFrom = From;
    // A non-table constant a register folded to is kept only as a last resort:
    // the table-base register may have been REUSED earlier in the block as a
    // loop-carried value (e.g. `mov %rdx,%r8` overwriting an accumulator that
    // also lived in r8), so foldRegConstant can emulate the stale accumulator
    // value (which may happen to land in a mapped segment).  When that fold is
    // not a code-pointer table, keep following the def chain (the `mov` COPY to
    // the real base) and only fall back to it if the chain yields no table.
    std::optional<uint64_t> RegFallback;
    for (int G = 0; G < limits::kMaxSliceDepth; ++G) {
      if (Cur.isConst())
        return Cur.Offset;
      // A table base materialised in a dominator (`lea`/`adrp+add`/`leal
      // GOTOFF`/`pc+litpool`) folds via prefix emulation; a runtime copy of it
      // in the loop body does not (the emulator halts at the loop back-edge),
      // so try each register along the COPY chain and take the first that
      // folds.  Once the def chain moves before a register overwrite, emulate
      // only up to that earlier use point; using the select's cutoff throughout
      // would observe the newer register value and can mistake the other table
      // arm for this one.
      if (Cur.isReg()) {
        va_t FoldCutoff = Cutoff;
        if (CurFrom >= 0 && CurFrom < static_cast<int>(Ops.size()))
          FoldCutoff = std::min(FoldCutoff, Ops[CurFrom].Addr);
        auto F = foldRegConstant(Img, Rec, Cur.Offset, FoldCutoff);
        if (F && *F) {
          if (TableEntW == 0 || countCodePtrRelocRun(Img, *F, TableEntW) > 0)
            return F;
          if (!RegFallback)
            RegFallback = F;
        }
      }
      int D = reachingDefIdx(Ops, CurFrom, Cur);
      if (D < 0)
        break;
      const LowOp &O = Ops[D];
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1) {
        Cur = O.Inputs[0];
        CurFrom = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0) {
        auto F = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        if (!F)
          break;
        return truncateToByteWidth(*F, O.Output.Size);
      }
      if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
        auto A = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        auto B = foldArm(O.Inputs[1], D - 1, Cutoff, Depth + 1);
        if (A && B) {
          return truncateToByteWidth(*A + *B, O.Output.Size);
        }
        // i386 PIC materialises a table base as GOT_base + GOTOFF.  GOT_base
        // is legitimately zero in the relocatable image model, but
        // foldRegConstant deliberately rejects a zero fold, leaving just the
        // GOTOFF addend here.  Accept that addend only when the image proves it
        // starts a code-pointer relocation run; this keeps an unrelated
        // partially-folded add from being mistaken for a table address.
        if (!A && B && TableEntW != 0 &&
            countCodePtrRelocRun(Img, *B, TableEntW) > 0)
          return B;
        if (A && !B && TableEntW != 0 &&
            countCodePtrRelocRun(Img, *A, TableEntW) > 0)
          return A;
        break;
      }
      if (O.Opcode == NdOp::INT_SUB && O.NumInputs >= 2) {
        auto A = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        auto B = foldArm(O.Inputs[1], D - 1, Cutoff, Depth + 1);
        if (A && B) {
          return truncateToByteWidth(*A - *B, O.Output.Size);
        }
        break;
      }
      if (O.Opcode == NdOp::LOAD) {
        // Store-forward a spilled table base (i386 `cmov (%esp),...`).
        const NdVar &AddrV = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        uint64_t LBase = 0;
        int64_t LOff = 0;
        if (!frameSlotKey(Ops, D - 1, AddrV, TRI, LBase, LOff))
          break;
        for (int K = D - 1; K >= 0; --K) {
          const LowOp &S = Ops[K];
          if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
            continue;
          const NdVar &SAddr = (S.NumInputs >= 3) ? S.Inputs[1] : S.Inputs[0];
          const NdVar &SVal = (S.NumInputs >= 3) ? S.Inputs[2] : S.Inputs[1];
          uint64_t SB = 0;
          int64_t SO = 0;
          if (frameSlotKey(Ops, K - 1, SAddr, TRI, SB, SO) && SB == LBase &&
              SO == LOff)
            return foldArm(SVal, K - 1, Cutoff, Depth + 1);
        }
        break;
      }
      break;
    }
    return RegFallback;
  };

  // Resolve a select arm's underlying mask (for the `(A&M)|(B&~M)` blend form)
  // back through COPY chains; the negated arm's mask is defined by INT_NOT.
  auto maskIsNegated = [&](NdVar M, int From) -> bool {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      int D = reachingDefIdx(Ops, From, M);
      if (D < 0)
        return false;
      if (Ops[D].Opcode == NdOp::INT_NOT)
        return true;
      if (Ops[D].Opcode == NdOp::COPY && Ops[D].NumInputs >= 1) {
        M = Ops[D].Inputs[0];
        From = D - 1;
        continue;
      }
      return false;
    }
    return false;
  };

  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
    const LowOp &L = Ops[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    uint16_t W = L.Output.Size;
    if (W != 4 && W != 8)
      continue;
    TableEntW = W;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    int AddIdx = reachingDefIdx(Ops, I - 1, AddrV);
    for (int G = 0;
         AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
         Ops[AddIdx].NumInputs >= 1 &&
         (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
         G < limits::kMaxQuasiCopyDepth;
         ++G)
      AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    // The load address is `base + index`; the base is the runtime-selected
    // table pointer.  Try each operand as the base.
    for (int BaseW = 0; BaseW < 2; ++BaseW) {
      NdVar BaseV = Ops[AddIdx].Inputs[BaseW];
      NdVar IdxV = Ops[AddIdx].Inputs[1 - BaseW];
      int BDef = reachingDefIdx(Ops, AddIdx - 1, BaseV);
      for (int G = 0;
           BDef >= 0 && Ops[BDef].Opcode == NdOp::COPY &&
           Ops[BDef].NumInputs >= 1 &&
           (Ops[BDef].Inputs[0].isReg() || Ops[BDef].Inputs[0].isTemp()) &&
           G < limits::kMaxQuasiCopyDepth;
           ++G)
        BDef = reachingDefIdx(Ops, BDef - 1, Ops[BDef].Inputs[0]);
      if (BDef < 0)
        continue;
      const LowOp &BD = Ops[BDef];
      va_t Cutoff = BD.Addr;

      // The positive arm is the SELECT true input / the blend operand ANDed
      // with the base mask M; the negative arm is the SELECT false input / the
      // operand ANDed with ~M.
      NdVar ArmPos, ArmNeg;
      bool Matched = false;
      if (BD.Opcode == NdOp::SELECT && BD.NumInputs >= 3) {
        ArmPos = BD.Inputs[1];
        ArmNeg = BD.Inputs[2];
        Matched = true;
      } else if (BD.Opcode == NdOp::INT_OR && BD.NumInputs >= 2) {
        // Each OR input is INT_AND(table_arm, mask).  Split arm vs mask, then
        // classify by whether the mask is the negated one (~M).
        NdVar Arms[2], Masks[2];
        bool BlendOk = true;
        for (int Side = 0; Side < 2 && BlendOk; ++Side) {
          int AndD = reachingDefIdx(Ops, BDef - 1, BD.Inputs[Side]);
          for (int G = 0;
               AndD >= 0 && Ops[AndD].Opcode == NdOp::COPY &&
               Ops[AndD].NumInputs >= 1 && G < limits::kMaxQuasiCopyDepth;
               ++G)
            AndD = reachingDefIdx(Ops, AndD - 1, Ops[AndD].Inputs[0]);
          if (AndD < 0 || Ops[AndD].Opcode != NdOp::INT_AND ||
              Ops[AndD].NumInputs < 2) {
            BlendOk = false;
            break;
          }
          // The arm is the operand that folds to a code-pointer table; the
          // other is the select mask.
          int ArmWhich = -1;
          for (int W2 = 0; W2 < 2; ++W2) {
            auto Cand = foldArm(Ops[AndD].Inputs[W2], AndD - 1, Cutoff, 0);
            if (Cand && countCodePtrRelocRun(Img, *Cand, W) > 0) {
              ArmWhich = W2;
              break;
            }
          }
          if (ArmWhich < 0) {
            BlendOk = false;
            break;
          }
          Arms[Side] = Ops[AndD].Inputs[ArmWhich];
          Masks[Side] = Ops[AndD].Inputs[1 - ArmWhich];
        }
        if (BlendOk) {
          bool Neg0 = maskIsNegated(Masks[0], BDef - 1);
          bool Neg1 = maskIsNegated(Masks[1], BDef - 1);
          if (Neg0 != Neg1) {
            // Positive arm uses M (the non-negated mask).
            ArmPos = Neg0 ? Arms[1] : Arms[0];
            ArmNeg = Neg0 ? Arms[0] : Arms[1];
            Matched = true;
          }
        }
      }
      if (!Matched)
        continue;

      auto CposOpt = foldArm(ArmPos, BDef - 1, Cutoff, 0);
      auto CnegOpt = foldArm(ArmNeg, BDef - 1, Cutoff, 0);
      if (!CposOpt || !CnegOpt || *CposOpt == *CnegOpt)
        continue;
      uint64_t Cpos = *CposOpt, Cneg = *CnegOpt;
      uint64_t Lo = std::min(Cpos, Cneg), Hi = std::max(Cpos, Cneg);
      uint64_t Dbytes = Hi - Lo;
      if (Dbytes == 0 || Dbytes % W != 0)
        continue;
      uint32_t LoEntries = static_cast<uint32_t>(Dbytes / W);

      uint32_t RunLo = countCodePtrRelocRun(Img, Lo, W);
      uint32_t RunHi = countCodePtrRelocRun(Img, Hi, W);
      if (RunHi == 0)
        continue;

      uint64_t IdxReg = scaledIndexReg(Ops, AddIdx - 1, IdxV);
      if (IdxReg == InvalidVA)
        IdxReg = traceToRegister(Ops, AddIdx - 1, IdxV);

      // Adjacent tables merge into one contiguous table at Lo: the lower run
      // reaches the higher table and the higher run continues from there, so a
      // single base+offset scan over LoEntries + RunHi entries reads both.
      if (RunLo >= LoEntries + RunHi) {
        uint32_t Total = LoEntries + RunHi;
        if (Total < limits::kMinJumpTableEntries ||
            Total > limits::kMaxJumpTableEntries)
          continue;

        Info.BaseAddr = Lo;
        Info.EntrySize = W;
        Info.MaxEntries = Total;
        Info.RelocAbsolute = true;
        Info.RelocBounded = true;
        Info.IsRelative = false;
        Info.IsSigned = false;
        Info.IndexReg = IdxReg;
        Info.PreScaledIndex = true;
        Info.Stride = W;
        Info.TwoTableSelect = true;
        Info.TwoTableOffset = static_cast<uint32_t>(Dbytes);
        Info.TwoTableHiPositive = (Cpos > Cneg);
        LLVM_DEBUG(llvm::dbgs()
                   << "  two-table: merged tables 0x" << llvm::utohexstr(Lo)
                   << " + 0x" << llvm::utohexstr(Hi) << " (" << Total
                   << " entries, D=" << Dbytes << ")\n");
        return true;
      }

      // Non-adjacent tables: A and B occupy disjoint code-pointer runs (clang
      // did not pack them, or unrelated read-only data sits between).  They
      // cannot fold to a contiguous base, but each is a complete, equal-length
      // run, so read both and lay their targets out lower-table first
      // (positions [0,N) = table at Lo, [N,2N) = table at Hi).  The dispatch
      // then lowers to the same `idx + (positiveArm ? D : 0)` switch as the
      // adjacent form, with the offset D expressed in the concatenated
      // coordinate (N entries) rather than the memory distance Hi-Lo.  This is
      // sound because the runtime target is always one of the 2N read
      // code-pointer entries, whichever base the runtime select chose.
      auto readCodePtrRun = [&](va_t Base, uint32_t Count,
                                std::vector<va_t> &Out) -> bool {
        for (uint32_t I = 0; I < Count; ++I) {
          const uint8_t *P =
              Img.readVA(Base + static_cast<uint64_t>(I) * W, W);
          if (!P)
            return false;
          va_t Target = 0;
          std::memcpy(&Target, P, W); // absolute code pointer (post-link VA)
          if (!isValidTarget(Img, Target, CurrentFuncEntry))
            return false;
          Out.push_back(Target);
        }
        return true;
      };

      if (RunLo < limits::kMinJumpTableEntries || RunLo != RunHi)
        continue;
      uint32_t N = RunLo;
      if (2u * N > limits::kMaxJumpTableEntries)
        continue;
      std::vector<va_t> Union;
      Union.reserve(2u * N);
      if (!readCodePtrRun(Lo, N, Union) || !readCodePtrRun(Hi, N, Union))
        continue;

      Info.BaseAddr = Lo;
      Info.EntrySize = W;
      Info.MaxEntries = 2u * N;
      Info.RelocAbsolute = true;
      Info.RelocBounded = true;
      Info.IsRelative = false;
      Info.IsSigned = false;
      Info.IndexReg = IdxReg;
      Info.PreScaledIndex = true;
      Info.Stride = W;
      Info.TwoTableSelect = true;
      Info.TwoTableOffset = N * W; // concatenated (lo-first) coordinate
      Info.TwoTableHiPositive = (Cpos > Cneg);
      Info.ExplicitTargets = std::move(Union);
      LLVM_DEBUG(llvm::dbgs()
                 << "  two-table: non-adjacent tables 0x" << llvm::utohexstr(Lo)
                 << " + 0x" << llvm::utohexstr(Hi) << " (" << (2u * N)
                 << " targets, N=" << N << ")\n");
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// tryConstBaseAbsoluteTable — constant-base absolute table, decoupled load
//===----------------------------------------------------------------------===//

bool CFGBuilder::tryConstBaseAbsoluteTable(const BinaryImage &Img,
                                           const InsnRecord &Rec,
                                           JumpTableInfo &Info) {
  if (!CurrentImg)
    return false;
  bool HasIndBranch = false;
  bool BranchHasOwnLoad = false;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1)
      HasIndBranch = true;
    if (Op.Opcode == NdOp::LOAD)
      BranchHasOwnLoad = true;
  }
  if (!HasIndBranch)
    return false;
  // The single-instruction table (`jmp *tab(,idx,W)`) is recovered by the
  // backward slice over the branch's own ops; only handle the *decoupled* form
  // here, where an -O0 spill/reload relay puts the table load in a different
  // instruction than the branch.  Gating on the absence of a load in the branch
  // record keeps this strictly additive — it never re-routes a shape another
  // strategy already resolves.
  if (BranchHasOwnLoad)
    return false;

  // Flatten the whole function prefix up to and including the dispatch so a
  // table load in any predecessor goto-site is in scope — this covers both a
  // single decoupled relay (one predecessor) and a shared multi-site dispatch
  // (several goto-site predecessors all reading one common table).
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  // Recover the concrete scale of a scaled-index operand (INT_MULT const /
  // INT_LEFT shift), traced through value-preserving reshapes.  Returns 0 when
  // the operand is not a scaled index.
  auto scaleOf = [&](NdVar V, int From) -> uint32_t {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      if (!V.isReg() && !V.isTemp())
        return 0;
      int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        return 0;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::INT_MULT && O.NumInputs >= 2 &&
          O.Inputs[1].isConst())
        return static_cast<uint32_t>(O.Inputs[1].Offset);
      if (O.Opcode == NdOp::INT_LEFT && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset < 6)
        return 1u << O.Inputs[1].Offset;
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      return 0;
    }
    return 0;
  };

  // Scan backward for the table load: a LOAD of pointer width whose address is
  // `const_base + idx*W` (W == the load width) and whose base carries a run of
  // absolute code-pointer relocations (the verifiable label-table signature).
  // The nearest such load to the dispatch wins; a shared multi-site dispatch
  // reads one common base, so any site's load recovers the same table.
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
    const LowOp &L = Ops[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    uint16_t W = L.Output.Size;
    if (W != 4 && W != 8)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    int AddIdx = reachingDefIdx(Ops, I - 1, AddrV);
    for (int G = 0;
         AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
         Ops[AddIdx].NumInputs >= 1 &&
         (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
         G < limits::kMaxQuasiCopyDepth;
         ++G)
      AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    for (int Side = 0; Side < 2; ++Side) {
      uint64_t Idx = scaledIndexReg(Ops, AddIdx - 1, Ops[AddIdx].Inputs[Side]);
      if (Idx == InvalidVA)
        continue;
      if (scaleOf(Ops[AddIdx].Inputs[Side], AddIdx - 1) != W)
        continue; // the scale must be the entry width for an absolute table

      // The other operand is the table base: a constant data VA, either a
      // literal (`disp(,idx,W)`) or a register folded to one (`lea tab,%rN`).
      const NdVar &BaseV = Ops[AddIdx].Inputs[1 - Side];
      va_t Base = 0;
      if (BaseV.isConst())
        Base = static_cast<va_t>(BaseV.Offset);
      else if (BaseV.isReg() || BaseV.isTemp()) {
        uint64_t BReg = traceToRegister(Ops, AddIdx - 1, BaseV);
        if (BReg != InvalidVA)
          if (auto F = foldRegConstant(Img, Rec, BReg, L.Addr); F && *F)
            Base = static_cast<va_t>(*F);
      }
      if (Base == 0)
        continue;
      const auto *Seg = Img.getSegmentFor(Base);
      if (!Seg || Seg->Data.empty())
        continue;
      uint32_t Run = countCodePtrRelocRun(Img, Base, W);
      if (Run < limits::kMinJumpTableEntries)
        continue;

      uint64_t IdxSrc = traceRegSource(Ops, AddIdx - 1, Idx);
      Info.BaseAddr = Base;
      Info.EntrySize = W;
      Info.IsRelative = false;
      Info.IsSigned = false;
      Info.IndexReg = (IdxSrc != InvalidVA) ? IdxSrc : Idx;
      // The absolute code-pointer relocation run is the exact entry count and
      // the label-table signature; a computed goto carries no comparison guard,
      // so bound it here (RelocAbsolute skips the guard search downstream, which
      // could only mis-bound it).
      Info.MaxEntries = Run;
      Info.RelocAbsolute = true;
      Info.RelocBounded = true;
      LLVM_DEBUG(llvm::dbgs()
                 << "  const-base-abs: decoupled absolute table 0x"
                 << llvm::utohexstr(Base) << " (W=" << W << ", " << Run
                 << " entries)\n");
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// tryTwoLevelIndexTable — index-byte (MSVC-style) two-level table
//===----------------------------------------------------------------------===//

/// Count the run of consecutive relocation slots in \p Slots starting at
/// \p TableAddr, stepping by \p EntrySize.  Mirrors the code/rel-code run
/// counters in JumpTableResolver.cpp for a locally-supplied slot set.
static uint32_t relocRunIn(const std::set<uint64_t> &Slots, va_t TableAddr,
                           uint16_t EntrySize) {
  if (EntrySize == 0 || Slots.empty())
    return 0;
  uint32_t Run = 0;
  for (va_t VA = TableAddr; Run < limits::kMaxJumpTableEntries;
       VA += EntrySize) {
    if (!Slots.count(VA))
      break;
    ++Run;
  }
  return Run;
}

/// Decompose the address of an *index-table* load (`idxtab + switchvar[*s1]`)
/// into its constant table base and the index register.  Unlike
/// analyzeTableLoadAddr this tolerates an unscaled index (a byte index table
/// has scale 1, so there is no INT_MULT/INT_LEFT to key on) and folds one
/// operand to a constant read-only VA to identify the table base.  Returns
/// true and sets \p TableAddr (folded base), \p IndexReg (traced to a plain
/// register), and \p Scale (1 or the entry width) on success.
bool CFGBuilder::decomposeIndexTableLoadAddr(const BinaryImage &Img,
                                             const InsnRecord &Rec,
                                             const std::vector<LowOp> &Ops,
                                             int LoadIdx, uint16_t EntryWidth,
                                             va_t &TableAddr, uint64_t &IndexReg,
                                             uint32_t &Scale) const {
  if (LoadIdx <= 0 || LoadIdx >= static_cast<int>(Ops.size()))
    return false;
  const LowOp &L = Ops[LoadIdx];
  const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
  int AddIdx = reachingDefIdx(Ops, LoadIdx - 1, AddrV);
  for (int G = 0; AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
                  Ops[AddIdx].NumInputs >= 1 &&
                  (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
                  G < limits::kMaxQuasiCopyDepth;
       ++G)
    AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
  if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddIdx].NumInputs < 2)
    return false;
  va_t LoadAddr = L.Addr;

  // One operand is the (constant / foldable) table base; the other is the
  // switch-variable index, optionally scaled by the entry width.
  for (int BaseW = 0; BaseW < 2; ++BaseW) {
    const NdVar &BaseV = Ops[AddIdx].Inputs[BaseW];
    const NdVar &IdxV = Ops[AddIdx].Inputs[1 - BaseW];

    va_t Base = 0;
    if (BaseV.isConst()) {
      Base = BaseV.Offset;
    } else if (BaseV.isReg() || BaseV.isTemp()) {
      uint64_t BaseReg = traceToRegister(Ops, AddIdx - 1, BaseV);
      if (BaseReg == InvalidVA)
        continue;
      auto Folded = foldRegConstant(Img, Rec, BaseReg, LoadAddr);
      if (!Folded || *Folded == 0)
        continue;
      Base = *Folded;
    } else {
      continue;
    }
    if (Base == 0 || !Img.getSegmentFor(Base))
      continue;

    // The index may be scaled (halfword index table: `idx*2`) or plain (byte
    // index table: scale 1).  Require the scale to equal the entry width.
    uint32_t S = 1;
    uint64_t IdxReg = scaledIndexReg(Ops, AddIdx - 1, IdxV);
    if (IdxReg != InvalidVA) {
      // Recover the concrete scale so it can be validated against EntryWidth.
      int SD = reachingDefIdx(Ops, AddIdx - 1, IdxV);
      for (int G = 0; SD >= 0 &&
                      (Ops[SD].Opcode == NdOp::COPY ||
                       Ops[SD].Opcode == NdOp::INT_ZEXT ||
                       Ops[SD].Opcode == NdOp::INT_SEXT) &&
                      Ops[SD].NumInputs >= 1 && G < limits::kMaxQuasiCopyDepth;
           ++G)
        SD = reachingDefIdx(Ops, SD - 1, Ops[SD].Inputs[0]);
      if (SD < 0)
        continue;
      if (Ops[SD].Opcode == NdOp::INT_MULT && Ops[SD].NumInputs >= 2 &&
          Ops[SD].Inputs[1].isConst())
        S = static_cast<uint32_t>(Ops[SD].Inputs[1].Offset);
      else if (Ops[SD].Opcode == NdOp::INT_LEFT && Ops[SD].NumInputs >= 2 &&
               Ops[SD].Inputs[1].isConst() && Ops[SD].Inputs[1].Offset < 6)
        S = 1u << Ops[SD].Inputs[1].Offset;
      else
        continue;
    } else {
      IdxReg = traceToRegister(Ops, AddIdx - 1, IdxV);
      if (IdxReg == InvalidVA)
        continue;
    }
    if (S != EntryWidth)
      continue;

    TableAddr = Base;
    IndexReg = IdxReg;
    Scale = S;
    return true;
  }
  return false;
}

bool CFGBuilder::tryTwoLevelIndexTable(const BinaryImage &Img,
                                       const InsnRecord &Rec,
                                       JumpTableInfo &Info) {
  if (!CurrentImg)
    return false;
  bool HasIndBranch = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  if (!HasIndBranch)
    return false;

  // Flatten the dispatch block plus its single-predecessor path so both chained
  // loads (the index-table load in a predecessor goto-site block and the
  // address-table load at the branch) are visible to one backward scan.
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> Ops = collectPathOps(BlkStart, Rec.Addr);
  if (Ops.empty())
    return false;

  // 1) Locate the address-table (jmptab) load: the last pointer-width scaled
  //    load feeding the branch, `jmptab + entryIdx*W2`.
  uint64_t JmpBaseReg = InvalidVA, EntryIdxReg = InvalidVA;
  uint16_t W2 = 0;
  int JmpLoadIdx = -1;
  {
    uint64_t Disp = 0;
    bool Scaled = false;
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
      const LowOp &L = Ops[I];
      if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
        continue;
      uint16_t W = L.Output.Size;
      if (W != 4 && W != 8)
        continue;
      const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
      if (!AddrV.isReg() && !AddrV.isTemp())
        continue;
      if (analyzeTableLoadAddr(Ops, I - 1, AddrV, JmpBaseReg, EntryIdxReg,
                               Scaled, Disp) &&
          Scaled) {
        W2 = W;
        JmpLoadIdx = I;
        break;
      }
    }
  }
  if (JmpLoadIdx < 0 || EntryIdxReg == InvalidVA || W2 == 0)
    return false;

  // 2) The jmptab index must itself be the *value loaded* by a compact
  //    byte/halfword index-table load — trace it (through value-preserving
  //    reshapes only) to a LOAD of width 1 or 2.  Anything else (arithmetic on
  //    the index, a plain register) is not a two-level table.
  int IdxLoadIdx = -1;
  uint16_t W1 = 0;
  {
    NdVar V = NdVar::reg(EntryIdxReg, 8);
    int From = JmpLoadIdx - 1;
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        break;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::LOAD) {
        W1 = O.Output.Size;
        IdxLoadIdx = D;
        break;
      }
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
          (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      break;
    }
  }
  // A byte/halfword index table is the hallmark of the compaction; a wider
  // "index" is indistinguishable from an ordinary single-level table entry.
  if (IdxLoadIdx < 0 || (W1 != 1 && W1 != 2))
    return false;

  // 3) Decompose the index-table load address into idxtab base + switch var.
  va_t IdxTab = 0;
  uint64_t SwitchIdxReg = InvalidVA;
  uint32_t IdxScale = 1;
  if (!decomposeIndexTableLoadAddr(Img, Rec, Ops, IdxLoadIdx, W1, IdxTab,
                                   SwitchIdxReg, IdxScale))
    return false;

  // 4) Fold the address-table base and confirm it is distinct from idxtab.
  va_t JmpTab = 0;
  {
    va_t FoldAt = Ops[JmpLoadIdx].Addr;
    auto Folded = foldRegConstant(Img, Rec, JmpBaseReg, FoldAt);
    if (!Folded || *Folded == 0)
      return false;
    JmpTab = *Folded;
  }
  if (JmpTab == IdxTab)
    return false;
  const auto *JmpSeg = Img.getSegmentFor(JmpTab);
  const auto *IdxSeg = Img.getSegmentFor(IdxTab);
  if (!JmpSeg || JmpSeg->Data.empty() || !IdxSeg || IdxSeg->Data.empty())
    return false;
  // The index table lives in read-only data; a writable/executable "idxtab"
  // would not be a compiler-emitted constant index table.
  if (IdxSeg->isWritable() || IdxSeg->isExecutable())
    return false;

  // 5) The address table's signature: a run of loader-applied code-pointer
  //    relocations (absolute) or PC-relative-to-code relocations (relative).
  //    The run length M is the exact address-table entry count, and every
  //    idxtab byte must be < M — the constraint that distinguishes a genuine
  //    two-level table from an unrelated pair of chained loads.
  bool Relative = false;
  uint32_t M = relocRunIn(Img.CodePtrRelocSlots, JmpTab, W2);
  if (M < limits::kMinJumpTableEntries) {
    uint32_t RM = relocRunIn(Img.RelCodeRelocSlots, JmpTab, W2);
    if (RM >= limits::kMinJumpTableEntries) {
      M = RM;
      Relative = true;
    }
  }
  if (M < limits::kMinJumpTableEntries)
    return false;

  // Flatten the whole function prefix so the outer range guard and any
  // comparison of the loaded index value are both visible.
  std::vector<LowOp> Pre;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Pre.push_back(Op);

  // Locate the index-table load in the function-prefix ops (by address and
  // output nd-var) so the discriminator below can reason about its result.
  int IdxLoadInPre = -1;
  {
    va_t L1Addr = Ops[IdxLoadIdx].Addr;
    const NdVar &L1Out = Ops[IdxLoadIdx].Output;
    for (int I = 0; I < static_cast<int>(Pre.size()); ++I)
      if (Pre[I].Opcode == NdOp::LOAD && Pre[I].Addr == L1Addr &&
          Pre[I].Output.Space == L1Out.Space &&
          Pre[I].Output.Offset == L1Out.Offset &&
          Pre[I].Output.Size == L1Out.Size &&
          Pre[I].Output.isTemp() == L1Out.isTemp())
        IdxLoadInPre = I; // last match at that address wins
  }

  // Discriminator — distinguish a genuine two-level index table from an
  // ordinary `switch(user_array[i])`, which lowers to the *identical* shape
  // (load a value, then index the compiler's jump table by it).  In the latter
  // the loaded value IS the switch variable and is range-guarded as the switch
  // condition (`cmp k, hi; ja default`); dispatching on the outer array index
  // would be wrong.  A compiler-generated index table's value, by contrast, is
  // an opaque index used *only* to address the address table and is never
  // compared.  So bail when the idxtab-loaded value reaches a constant
  // comparison: that marks it as the real switch variable (single-level).
  if (IdxLoadInPre >= 0) {
    auto tracesToIdxLoad = [&](NdVar V, int From) -> bool {
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!V.isReg() && !V.isTemp())
          return false;
        int D = reachingDefIdx(Pre, From, V);
        if (D < 0)
          return false;
        if (D == IdxLoadInPre)
          return true;
        const LowOp &O = Pre[D];
        if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
             O.Opcode == NdOp::INT_SEXT) &&
            O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
            O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
            (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        return false;
      }
      return false;
    };
    for (int I = 0; I < static_cast<int>(Pre.size()); ++I) {
      const LowOp &Op = Pre[I];
      bool IsCompare =
          Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_SLESS ||
          Op.Opcode == NdOp::INT_LESSEQUAL ||
          Op.Opcode == NdOp::INT_SLESSEQUAL || Op.Opcode == NdOp::INT_EQUAL ||
          Op.Opcode == NdOp::INT_NOTEQUAL || Op.Opcode == NdOp::INT_SUB;
      if (!IsCompare || Op.NumInputs < 2)
        continue;
      int CW = Op.Inputs[1].isConst() ? 1 : (Op.Inputs[0].isConst() ? 0 : -1);
      if (CW < 0)
        continue;
      if (tracesToIdxLoad(Op.Inputs[1 - CW], I - 1))
        return false; // loaded value is the switch variable — single-level
    }
  }

  // 6) Bound the number of switch cases (idxtab length).  Prefer an explicit
  //    range guard on the switch variable; otherwise self-bound by the idxtab
  //    entries themselves (each must index a valid jmptab slot).
  //
  // Anchor the switch-variable trace at the index-table load: the register that
  // addresses idxtab (e.g. `rax`) is routinely *reused* after the load to hold
  // the loaded index byte's zero-extension, so tracing from the end of the op
  // list would follow that later reuse to the byte value instead of the real
  // switch variable.  Its reaching definition at the load is the true switch
  // variable (the guarded `x` copied into the address register).
  uint64_t SwitchSrc = traceRegSource(Ops, IdxLoadIdx - 1, SwitchIdxReg);
  uint32_t GuardBound = 0;
  {
    GuardBound = findBestBound(Pre, 0, SwitchSrc);
    if (GuardBound == 0 && SwitchSrc != SwitchIdxReg)
      GuardBound = findBestBound(Pre, 0, SwitchIdxReg);
    if (GuardBound > 0 && GuardBound < limits::kMaxJumpTableEntries &&
        guardUsesInclusiveCompare(Rec, SwitchSrc, GuardBound))
      GuardBound += 1;
  }

  uint32_t IdxCap =
      static_cast<uint32_t>((IdxSeg->Data.size() -
                             static_cast<size_t>(IdxTab - IdxSeg->VA)) /
                            W1);
  uint32_t Scan = std::min(IdxCap, limits::kMaxJumpTableEntries);
  if (GuardBound > 0)
    Scan = std::min(Scan, GuardBound);

  // 7) Compose one target per switch value: idxtab[v] indexes jmptab.
  std::vector<va_t> Targets;
  Targets.reserve(std::min<uint32_t>(Scan, 64));
  for (uint32_t V = 0; V < Scan; ++V) {
    const uint8_t *IP = Img.readVA(IdxTab + static_cast<uint64_t>(V) * W1, W1);
    if (!IP)
      break;
    uint32_t Iidx = 0;
    std::memcpy(&Iidx, IP, W1);
    if (Iidx >= M)
      break; // out of the address-table run — past the index table's end
    const uint8_t *EP =
        Img.readVA(JmpTab + static_cast<uint64_t>(Iidx) * W2, W2);
    if (!EP)
      break;
    va_t Target = 0;
    if (Relative) {
      int64_t Off = 0;
      if (W2 == 4) {
        int32_t V32;
        std::memcpy(&V32, EP, 4);
        Off = V32;
      } else {
        int64_t V64;
        std::memcpy(&V64, EP, 8);
        Off = V64;
      }
      Target = static_cast<va_t>(static_cast<int64_t>(JmpTab) + Off);
    } else {
      std::memcpy(&Target, EP, W2);
    }
    if (!isValidTarget(Img, Target, CurrentFuncEntry))
      break;
    Targets.push_back(Target);
  }

  if (Targets.size() < limits::kMinJumpTableEntries)
    return false;

  Info.BaseAddr = JmpTab;
  Info.EntrySize = W2;
  Info.IsRelative = Relative;
  Info.IsSigned = Relative;
  Info.IndexReg = SwitchSrc;
  Info.TwoLevelIndex = true;
  Info.ExplicitTargets = std::move(Targets);
  LLVM_DEBUG(llvm::dbgs()
             << "  two-level: idxtab 0x" << llvm::utohexstr(IdxTab)
             << " (W1=" << W1 << ") -> jmptab 0x" << llvm::utohexstr(JmpTab)
             << " (W2=" << W2 << ", M=" << M << "), "
             << Info.ExplicitTargets.size() << " cases\n");
  return true;
}

} // namespace neverd
