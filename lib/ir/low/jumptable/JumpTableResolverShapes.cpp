//===- JumpTableResolverShapes.cpp - Composite table shapes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizers for composite and decoupled single-level jump-table layouts:
/// runtime-selected two-table dispatch, where one branch picks between two
/// adjacent code-pointer tables at run time, and constant-base absolute tables
/// whose load is decoupled from the branch by an -O0 spill/reload relay or a
/// shared multi-site computed-goto dispatch.  The two-level index-byte table
/// lives in JumpTableResolverTwoLevel.cpp.
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
          const uint8_t *P = Img.readVA(Base + static_cast<uint64_t>(I) * W, W);
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
      // so bound it here (RelocAbsolute skips the guard search downstream,
      // which could only mis-bound it).
      Info.MaxEntries = Run;
      Info.RelocAbsolute = true;
      Info.RelocBounded = true;
      LLVM_DEBUG(llvm::dbgs() << "  const-base-abs: decoupled absolute table 0x"
                              << llvm::utohexstr(Base) << " (W=" << W << ", "
                              << Run << " entries)\n");
      return true;
    }
  }
  return false;
}

} // namespace neverd
