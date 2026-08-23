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
#include <limits>
#include <map>
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

  // i386 effective addresses are represented in the common internal VA width
  // after the guest-pointer arithmetic has completed.  That canonical
  // widening is not a frame epoch change, but the legacy frameSlotKey helper
  // intentionally does not traverse width changes because several older
  // authorization paths still consume it directly.  Peel the widening only
  // for this TwoTable spill candidate; the final point-sensitive load-role
  // certificate remains authoritative for publication.
  auto twoTableFrameSlotKey = [&](NdVar Address, int From, uint64_t &Base,
                                  int64_t &Offset) {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (Address.isReg() && TRI.isFrameReg(Address.Offset))
        break;
      if (!Address.isReg() && !Address.isTemp())
        break;
      const int D = reachingDefIdx(Ops, From, Address);
      if (D < 0)
        break;
      const LowOp &Def = Ops[D];
      if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
        Address = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Def.Opcode == NdOp::INT_ZEXT && Def.NumInputs >= 1 &&
          Def.Inputs[0].Size == Img.getPointerSize() &&
          Def.Output.Size > Def.Inputs[0].Size) {
        Address = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      break;
    }
    return frameSlotKey(Ops, From, Address, TRI, Base, Offset);
  };

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
        if (F && Img.getSegmentFor(*F)) {
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
        if (!twoTableFrameSlotKey(AddrV, D - 1, LBase, LOff))
          break;
        for (int K = D - 1; K >= 0; --K) {
          const LowOp &S = Ops[K];
          if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
            continue;
          const NdVar &SAddr = (S.NumInputs >= 3) ? S.Inputs[1] : S.Inputs[0];
          const NdVar &SVal = (S.NumInputs >= 3) ? S.Inputs[2] : S.Inputs[1];
          uint64_t SB = 0;
          int64_t SO = 0;
          if (twoTableFrameSlotKey(SAddr, K - 1, SB, SO) && SB == LBase &&
              SO == LOff)
            return foldArm(SVal, K - 1, Cutoff, Depth + 1);
        }
        break;
      }
      break;
    }
    return RegFallback;
  };

  // Resolve a blend arm's mask through COPY chains.  A valid pointer select
  // uses an all-zero/all-ones mask produced by INT_NEG2(condition) and its
  // exact INT_NOT complement; arbitrary complementary bit masks could splice
  // the two addresses into a third pointer and are not a table-base select.
  struct MaskDefinition {
    int Kind = -1; // 0 = INT_NEG2 base mask, 1 = INT_NOT complement
    int OpIndex = -1;
  };
  auto maskDefinition = [&](NdVar M, int From) -> MaskDefinition {
    for (int G = 0; G < limits::kMaxQuasiCopyDepth; ++G) {
      int D = reachingDefIdx(Ops, From, M);
      if (D < 0)
        return {};
      if (Ops[D].Opcode == NdOp::INT_NOT)
        return {1, D};
      if (Ops[D].Opcode == NdOp::INT_NEG2)
        return {0, D};
      if (Ops[D].Opcode == NdOp::COPY && Ops[D].NumInputs >= 1) {
        M = Ops[D].Inputs[0];
        From = D - 1;
        continue;
      }
      return {};
    }
    return {};
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
    for (int G = 0; AddIdx >= 0 && G < limits::kMaxQuasiCopyDepth; ++G) {
      const LowOp &Forwarder = Ops[AddIdx];
      if (Forwarder.NumInputs < 1 ||
          (!Forwarder.Inputs[0].isReg() && !Forwarder.Inputs[0].isTemp()))
        break;
      const bool IsCopy = Forwarder.Opcode == NdOp::COPY;
      const bool IsCanonicalGuestAddressWiden =
          Forwarder.Opcode == NdOp::INT_ZEXT &&
          Forwarder.Inputs[0].Size == Img.getPointerSize() &&
          Forwarder.Output.Size >= Forwarder.Inputs[0].Size;
      if (!IsCopy && !IsCanonicalGuestAddressWiden)
        break;
      AddIdx = reachingDefIdx(Ops, AddIdx - 1, Forwarder.Inputs[0]);
    }
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
      bool MatchedSelect = false;
      bool MatchedBlend = false;
      int BlendAndIdx[2] = {-1, -1};
      int BlendArmSide[2] = {-1, -1};
      MaskDefinition BlendMaskDef[2];
      int PositiveBlendSide = -1;
      if (BD.Opcode == NdOp::SELECT && BD.NumInputs >= 3) {
        ArmPos = BD.Inputs[1];
        ArmNeg = BD.Inputs[2];
        Matched = true;
        MatchedSelect = true;
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
          BlendAndIdx[Side] = AndD;
          BlendArmSide[Side] = ArmWhich;
        }
        if (BlendOk) {
          BlendMaskDef[0] = maskDefinition(Masks[0], BDef - 1);
          BlendMaskDef[1] = maskDefinition(Masks[1], BDef - 1);
          if (BlendMaskDef[0].Kind >= 0 && BlendMaskDef[1].Kind >= 0 &&
              BlendMaskDef[0].Kind != BlendMaskDef[1].Kind) {
            // Positive arm uses M (the non-negated mask).
            const bool Neg0 = BlendMaskDef[0].Kind == 1;
            PositiveBlendSide = Neg0 ? 1 : 0;
            ArmPos = Neg0 ? Arms[1] : Arms[0];
            ArmNeg = Neg0 ? Arms[0] : Arms[1];
            Matched = true;
            MatchedBlend = true;
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

      uint32_t RunLo = countCodePtrRelocRun(Img, Lo, W);
      uint32_t RunHi = countCodePtrRelocRun(Img, Hi, W);
      if (RunLo < limits::kMinJumpTableEntries ||
          RunHi < limits::kMinJumpTableEntries)
        continue;

      // A SELECT/blend of two relocation-backed table bases is a
      // distinguishing composite shape.  From this point onward, failure of
      // the exact index-domain, base-merge, address, or target certificate is a
      // hard fail for this dispatch; generic resolvers must not publish one arm
      // as a single table.
      Info.CompositeShapeClaimed = true;

      NdVar ExactIndex;
      va_t IndexUseAddr = InvalidVA;
      int IndexUseSeq = -1;
      bool IndexIsPreScaled = false;
      uint64_t IdxReg = scaledIndexReg(Ops, AddIdx - 1, IdxV, &ExactIndex,
                                       &IndexUseAddr, &IndexUseSeq);
      if (IdxReg == InvalidVA) {
        // Size optimizers commonly fold `slot * W` into one bit mask, e.g.
        // `(x >> 5) & 0x38` for an eight-entry pointer table.  There is then no
        // MULT/LEFT for scaledIndexReg to find: the exact value consumed by the
        // address ADD is already the byte offset.  Keep that coordinate
        // explicit instead of pretending it is a logical slot index and
        // multiplying by W a second time in the address certificate.
        if ((!IdxV.isReg() && !IdxV.isTemp()) || IdxV.Size == 0)
          return false;
        ExactIndex = IdxV;
        IndexUseAddr = Ops[AddIdx].Addr;
        IndexUseSeq = Ops[AddIdx].Seq;
        IdxReg = traceToRegister(Ops, AddIdx - 1, IdxV);
        IndexIsPreScaled = true;
      }
      if (IdxReg == InvalidVA || ExactIndex.Size == 0 ||
          IndexUseAddr == InvalidVA || IndexUseSeq < 0)
        return false;
      // The x86 LowIR address container is eight bytes even for an i386
      // guest.  The effective-address coordinate is nevertheless the low
      // guest-pointer lane; carry that exact lane into both the mask-domain
      // and LOAD-role proofs so neither side invents a synthetic high half.
      if ((ExactIndex.isReg() || ExactIndex.isTemp()) &&
          Img.getPointerSize() != 0 && ExactIndex.Size > Img.getPointerSize())
        ExactIndex.Size = static_cast<uint16_t>(Img.getPointerSize());

      // Relocation-run length proves that slots contain code pointers; it does
      // not constrain the runtime selector.  Prove the common per-arm domain
      // from the exact logical index occurrence.  A power-of-two AND is a
      // whole-bit-domain proof (including wrap/negative bit patterns), unlike
      // the old finite-prefix evaluator.  Other domains remain fail-closed
      // until represented by the shared exact BoundEvidence lattice.
      RequestedCompleteJumpTableProof = true;
      if (!JumpTableProofContextComplete)
        return false;
      std::map<uint32_t, std::vector<JumpTableValueOccurrence>> MaskGroups;
      for (const auto &[Addr, Insn] : Insns) {
        if (Insn.IsInstructionGuard)
          continue;
        for (const LowOp &Mask : Insn.Ops) {
          if (Mask.Opcode != NdOp::INT_AND || Mask.NumInputs < 2 ||
              (!Mask.Output.isReg() && !Mask.Output.isTemp()))
            continue;
          int ConstantSide = Mask.Inputs[0].isConst()
                                 ? 0
                                 : (Mask.Inputs[1].isConst() ? 1 : -1);
          if (ConstantSide < 0)
            continue;
          const uint64_t M = Mask.Inputs[ConstantSide].Offset;
          uint64_t Bound64 = 0;
          if (IndexIsPreScaled) {
            // For an already-scaled byte coordinate the exact full domain is
            // `{0,W,...,(N-1)W}`.  An AND mask proves that domain precisely
            // when its low log2(W) bits are zero and the remaining quotient is
            // a contiguous power-of-two mask.  This is whole-bit-domain
            // reasoning, so negative inputs and modular wrap cannot re-enter
            // outside the stated set.
            if (W == 0 || M % W != 0 || M / W >= limits::kMaxJumpTableEntries)
              continue;
            Bound64 = M / W + 1;
          } else {
            if (M >= limits::kMaxJumpTableEntries)
              continue;
            Bound64 = M + 1;
          }
          if (!llvm::isPowerOf2_64(Bound64))
            continue;
          MaskGroups[static_cast<uint32_t>(Bound64)].push_back(
              {Mask.Output, Mask.Addr, Mask.Seq,
               /*DefinedAtPoint=*/true});
        }
      }
      uint32_t N = 0;
      std::vector<JumpTableValueOccurrence> MaskAlternatives;
      for (const auto &[Bound, Occurrences] : MaskGroups) {
        MaskAlternatives.insert(MaskAlternatives.end(), Occurrences.begin(),
                                Occurrences.end());
        const std::vector<bool> Match = tableValuesMatchAtUses(
            {{ExactIndex, IndexUseAddr, IndexUseSeq, MaskAlternatives,
              /*AllowZeroExtension=*/true,
              /*AllowSignExtension=*/false}});
        if (!Match.empty() && Match.front()) {
          N = Bound;
          break;
        }
      }
      if (N < limits::kMinJumpTableEntries ||
          N > limits::kMaxJumpTableEntries || RunLo < N || RunHi < N ||
          N > limits::kMaxJumpTableEntries / 2)
        return false;

      // Read exactly the proven N slots from each physical run.  Extra
      // relocations adjacent to either table are foreign data, not selector
      // domain evidence.  The logical selector concatenates the lower and
      // higher runs, regardless of their physical separation.
      auto readCodePtrRun = [&](va_t Base, uint32_t Count,
                                std::vector<va_t> &Out) -> bool {
        for (uint32_t I = 0; I < Count; ++I) {
          uint64_t Offset = 0;
          va_t Slot = 0;
          if (I != 0 && static_cast<uint64_t>(W) >
                            std::numeric_limits<uint64_t>::max() / I)
            return false;
          Offset = static_cast<uint64_t>(I) * W;
          if (Offset > std::numeric_limits<va_t>::max() - Base)
            return false;
          Slot = Base + Offset;
          const uint8_t *P = Img.readVA(Slot, W);
          if (!P)
            return false;
          va_t RawTarget = 0;
          std::memcpy(&RawTarget, P,
                      W); // absolute code pointer (post-link VA)
          std::optional<va_t> Canonical =
              canonicalizeAbsoluteTableCodeTarget(Img, RawTarget);
          if (!Canonical)
            return false;
          const va_t Target = *Canonical;
          if (!isValidTarget(Img, Target, CurrentFuncEntry))
            return false;
          Out.push_back(Target);
        }
        return true;
      };
      std::vector<va_t> Union;
      Union.reserve(2u * N);
      if (!readCodePtrRun(Lo, N, Union) || !readCodePtrRun(Hi, N, Union))
        return false;

      JumpTableValueOccurrence IndexOccurrence{ExactIndex, IndexUseAddr,
                                               IndexUseSeq,
                                               /*DefinedAtPoint=*/false};
      JumpTableValueOccurrence LoadOccurrence{L.Output, L.Addr, L.Seq,
                                              /*DefinedAtPoint=*/true};
      JumpTableLoadRole Role;
      Role.Load = LoadOccurrence;
      Role.LoadWidth = W;
      Role.AllowedBases = {Lo, Hi};
      Role.Indices = {IndexOccurrence};
      Role.AddressIndex = {IdxV, Ops[AddIdx].Addr, Ops[AddIdx].Seq,
                           /*DefinedAtPoint=*/false};
      Role.AddressScale = IndexIsPreScaled ? 1 : W;
      // scaledIndexReg deliberately records the logical selector at the input
      // of an address-width ZEXT.  The address-role proof must therefore be
      // allowed to follow that exact zero extension to the MULT/LEFT input;
      // the shared value matcher still rejects sign extension, truncation, or
      // any different reaching definition.
      Role.AllowZeroExtension =
          ExactIndex.Size != 0 && ExactIndex.Size < Img.getPointerSize();
      Role.SelectedBase = {BD.Output, BD.Addr, BD.Seq,
                           /*DefinedAtPoint=*/true};
      Role.TrueBase = Cpos;
      Role.FalseBase = Cneg;
      if (MatchedSelect) {
        Role.HasBaseSelect = true;
        Role.SelectCondition = {BD.Inputs[0], BD.Addr, BD.Seq,
                                /*DefinedAtPoint=*/false};
      } else if (MatchedBlend && PositiveBlendSide >= 0) {
        const int NegativeBlendSide = 1 - PositiveBlendSide;
        const LowOp &PositiveAnd = Ops[BlendAndIdx[PositiveBlendSide]];
        const LowOp &NegativeAnd = Ops[BlendAndIdx[NegativeBlendSide]];
        const LowOp &PositiveMask =
            Ops[BlendMaskDef[PositiveBlendSide].OpIndex];
        const LowOp &NegativeMask =
            Ops[BlendMaskDef[NegativeBlendSide].OpIndex];
        if (PositiveMask.Opcode != NdOp::INT_NEG2 ||
            PositiveMask.NumInputs < 1 ||
            NegativeMask.Opcode != NdOp::INT_NOT || NegativeMask.NumInputs < 1)
          return false;
        Role.HasBaseMaskBlend = true;
        Role.PositiveBlendArm = {PositiveAnd.Output, PositiveAnd.Addr,
                                 PositiveAnd.Seq,
                                 /*DefinedAtPoint=*/true};
        Role.NegativeBlendArm = {NegativeAnd.Output, NegativeAnd.Addr,
                                 NegativeAnd.Seq,
                                 /*DefinedAtPoint=*/true};
        Role.PositiveMask = {PositiveMask.Output, PositiveMask.Addr,
                             PositiveMask.Seq,
                             /*DefinedAtPoint=*/true};
        Role.NegativeMask = {NegativeMask.Output, NegativeMask.Addr,
                             NegativeMask.Seq,
                             /*DefinedAtPoint=*/true};
        Role.SelectCondition = {PositiveMask.Inputs[0], PositiveMask.Addr,
                                PositiveMask.Seq,
                                /*DefinedAtPoint=*/false};
        Role.PositiveBlendInputSide = static_cast<uint8_t>(PositiveBlendSide);
        Role.PositiveBaseInputSide =
            static_cast<uint8_t>(BlendArmSide[PositiveBlendSide]);
        Role.NegativeBaseInputSide =
            static_cast<uint8_t>(BlendArmSide[NegativeBlendSide]);
      } else {
        return false;
      }

      Info.setBaseAddr(Lo);
      Info.EntrySize = W;
      Info.MaxEntries = 2u * N;
      Info.PhysicalCapacity = 2u * N;
      Info.IndexDomainAuthenticated = true;
      Info.RelocAbsolute = true;
      Info.RelocBounded = true;
      Info.IsRelative = false;
      Info.IsSigned = false;
      Info.IndexReg = IdxReg;
      Info.IndexValueAtUse = ExactIndex;
      Info.IndexUseAddr = IndexUseAddr;
      Info.IndexUseSeq = IndexUseSeq;
      Info.IndexValueAlternatives = {IndexOccurrence};
      Info.PreScaledIndex = true;
      Info.Stride = W;
      Info.TwoTableSelect = true;
      Info.TwoTableOffset = N * W; // concatenated (lo-first) coordinate
      Info.TwoTableHiPositive = (Cpos > Cneg);
      Info.ExplicitTargets = std::move(Union);
      Info.TargetLoads = {LoadOccurrence};
      Info.LoadRoles = {std::move(Role)};
      Info.TableLoadAddr = L.Addr;
      Info.TableLoadSeq = L.Seq;
      Info.StorageRanges = {JumpTableStorageRange{Lo, W, W, N},
                            JumpTableStorageRange{Hi, W, W, N}};
      LLVM_DEBUG(llvm::dbgs()
                 << "  two-table: exact-domain tables 0x" << llvm::utohexstr(Lo)
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
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1)
      HasIndBranch = true;
  }
  if (!HasIndBranch)
    return false;

  // This occurrence-backed strategy handles both an in-instruction memory
  // jump (`jmp *tab(,idx,W)`) and a decoupled spill/reload relay.  The unified
  // load-address and load-output certificates below prevent an unrelated
  // prefix LOAD from being adopted merely because it has the same shape.

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
  bool FoundModel = false;
  va_t ModelBase = 0;
  uint16_t ModelWidth = 0;
  uint32_t ModelRun = 0;
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
         AddIdx >= 0 && Ops[AddIdx].NumInputs >= 1 &&
         (Ops[AddIdx].Opcode == NdOp::COPY ||
          (Ops[AddIdx].Opcode == NdOp::INT_ZEXT &&
           Ops[AddIdx].Output.Size == sizeof(va_t) &&
           Ops[AddIdx].Inputs[0].Size == Img.getPointerSize())) &&
         (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
         G < limits::kMaxQuasiCopyDepth;
         ++G)
      AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    for (int Side = 0; Side < 2; ++Side) {
      NdVar ExactIndex;
      va_t IndexUseAddr = InvalidVA;
      int IndexUseSeq = -1;
      uint64_t Idx = scaledIndexReg(Ops, AddIdx - 1, Ops[AddIdx].Inputs[Side],
                                    &ExactIndex, &IndexUseAddr, &IndexUseSeq);
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
          if (auto F = foldRegConstant(Img, Rec, BReg, L.Addr);
              F && Img.getSegmentFor(*F))
            Base = static_cast<va_t>(*F);
      }
      const auto *Seg = Img.getSegmentFor(Base);
      if (!Seg || Seg->Data.empty())
        continue;
      uint32_t Run = countCodePtrRelocRun(Img, Base, W);
      Run = boundCodePtrRunByNextAnchor(
          Img, Base, W, Run, currentRelocatedInstructionTableAnchors(Img));
      if (Run < limits::kMinJumpTableEntries)
        continue;

      // A shared -O0 dispatch may have one table LOAD in every predecessor,
      // with all targets spilled to the same frame slot and merged at one
      // INDIR_BR.  Collect every occurrence of one decode-identical table;
      // the publication proof then requires every feasible branch-target arm
      // to originate in this set.  A different-base load is not silently
      // adopted into the model and therefore makes an actual mixed merge fail
      // closed at the branch certificate.
      if (FoundModel &&
          (Base != ModelBase || W != ModelWidth || Run != ModelRun))
        continue;

      uint64_t IdxSrc = traceRegSource(Ops, AddIdx - 1, Idx);
      if (!FoundModel) {
        FoundModel = true;
        ModelBase = Base;
        ModelWidth = W;
        ModelRun = Run;
        Info.setBaseAddr(Base);
        Info.EntrySize = W;
        Info.IsRelative = false;
        Info.IsSigned = false;
        Info.IndexReg = (IdxSrc != InvalidVA) ? IdxSrc : Idx;
        Info.IndexValueAtUse = ExactIndex;
        Info.IndexUseAddr = IndexUseAddr;
        Info.IndexUseSeq = IndexUseSeq;
        Info.TableLoadAddr = L.Addr;
        Info.TableLoadSeq = L.Seq;
        // The relocation run authenticates storage and entry decoding only.
        // The exact runtime selector domain is established later by a guard,
        // mask, or complete modulo proof.
        Info.PhysicalCapacity = Run;
        Info.RelocAbsolute = true;
      }
      JumpTableValueOccurrence IndexOccurrence{ExactIndex, IndexUseAddr,
                                               IndexUseSeq,
                                               /*DefinedAtPoint=*/false};
      JumpTableValueOccurrence LoadOccurrence{L.Output, L.Addr, L.Seq,
                                              /*DefinedAtPoint=*/true};
      Info.IndexValueAlternatives.push_back(IndexOccurrence);
      Info.TargetLoads.push_back(LoadOccurrence);
      JumpTableLoadRole Role;
      Role.Load = LoadOccurrence;
      Role.LoadWidth = W;
      Role.AllowedBases = {Base};
      Role.Indices = {IndexOccurrence};
      Role.AddressScale = W;
      Info.LoadRoles.push_back(std::move(Role));
      LLVM_DEBUG(llvm::dbgs() << "  const-base-abs: decoupled absolute table 0x"
                              << llvm::utohexstr(Base) << " (W=" << W << ", "
                              << Run << " entries)\n");
      break;
    }
  }
  return FoundModel;
}

} // namespace neverd
