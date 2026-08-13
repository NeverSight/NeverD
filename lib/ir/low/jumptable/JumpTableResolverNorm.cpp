//===- JumpTableResolverNorm.cpp - Index normalization and stride ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovery of the transform that turns a switch variable into a table index:
/// the `sub base` / right-shift normalization anchored at the table load, and
/// the power-of-two stride implied by an AND mask on the switch variable.  The
/// recovered normalization is what pullBackBound inverts on a guard bound and
/// what recoverCaseLabels inverts on the resolved case labels.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// detectNormalization — detect index normalization (sub/shift) in the slice
//===----------------------------------------------------------------------===//

/// Walk the table-index computation backward from `V` (the index operand at the
/// table load), following value-preserving ops (COPY / ZEXT / SEXT /
/// SUBBYTES@0) and the stride/range mask (INT_AND), and record an
/// INT_SUB(x,const) base or INT_RIGHT/INT_ASHR(x,const) shift that genuinely
/// transforms the switch variable into the index.  Anchoring at the load keeps
/// a register the index is reused for *after* the table read (e.g. a peeled
/// loop iteration's accumulator) from contributing a phantom normalization.
static void traceIndexTransform(const std::vector<LowOp> &Ops, int FromIdx,
                                NdVar V, int64_t &NormBase, uint32_t &NormShift,
                                uint32_t &Stride) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (!V.isReg() && !V.isTemp())
      return;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return;
    const LowOp &Op = Ops[D];
    bool HasSrc =
        Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp());
    switch (Op.Opcode) {
    case NdOp::INT_SUB:
      if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() && HasSrc) {
        int64_t Base = static_cast<int64_t>(Op.Inputs[1].Offset);
        if (NormBase == 0 && Base > 0 && Base <= limits::kMaxNormBase)
          NormBase = Base;
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
      if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() && HasSrc) {
        uint32_t Shift = static_cast<uint32_t>(Op.Inputs[1].Offset);
        if (NormShift == 0 && Shift > 0 && Shift <= limits::kMaxNormShift)
          NormShift = Shift;
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (!HasSrc)
        return;
      V = Op.Inputs[0];
      FromIdx = D - 1;
      continue;
    case NdOp::INT_AND:
      // A mask on the switch variable preserves the surviving bits, so keep
      // tracing through it.  Trailing zero bits in the mask mean the index is
      // always a multiple of 2^k — a genuine stride — but only when this mask
      // is in the index dataflow (an unrelated `and x,6` in the prologue is
      // not).
      if (HasSrc && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
        uint64_t Mask = Op.Inputs[1].Offset;
        // A contiguous low-bits mask (2^k - 1) bounds the index to [0, 2^k):
        // the masked value *is* the terminal switch index, i.e. the source is
        // an explicit index expression like `(expr >> j) & m`, not a normalized
        // `switch(x)` whose case labels need reconstructing.  The ops feeding
        // the mask only *compute* that index, so they carry no label
        // normalization — stop here, leaving case labels as the raw indices
        // 0..N-1 the emitter's switch (which dispatches on the post-mask value)
        // actually compares. Without this the `>> j` inside the index
        // expression is mis-read as a NormShift and bogus `i << j` case values
        // are emitted that never match the runtime index (only the index-0 case
        // lands, the rest fall through to the default — observed on ARM32
        // inline PC-relative word tables, whose entries carry no relocation and
        // so skip the RelRun label reset).
        if (Mask != 0 && (Mask & (Mask + 1)) == 0) {
          NormBase = 0;
          NormShift = 0;
          return;
        }
        if (Stride <= 1 && Mask != 0) {
          uint32_t S = 1;
          uint64_t M = Mask;
          while ((M & 1) == 0 && S < limits::kMaxStrideScanBits) {
            M >>= 1;
            S <<= 1;
          }
          if (S > 1 && S <= limits::kMaxEntryBytes)
            Stride = S;
        }
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    case NdOp::SUBBYTES:
      if (HasSrc && Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Offset == 0) {
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    default:
      return;
    }
  }
}

void CFGBuilder::detectNormalization(const InsnRecord &Rec,
                                     JumpTableInfo &Info) {
  // Flatten the INDIR_BR block (inclusive of Rec) so an index computation
  // spanning several instructions is visible to the trace below.
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> BlockOps;
  for (auto It = Insns.lower_bound(BlkStart);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      BlockOps.push_back(Op);

  // Precise path: locate the table LOAD (scaled-index address) and trace the
  // index dataflow backward from it.  This restricts normalization/stride
  // detection to ops that actually transform the switch variable, so unrelated
  // arithmetic in the block (a shift carry-flag helper temp `t = t - 1`, a case
  // body's `and x,6`) cannot inject a phantom base/shift/stride.
  //
  // It also recovers the scaled-index register itself.  A plain absolute or
  // relative table folded straight out of the dispatch (`jmp *tab(,idx,8)` with
  // the base a constant in the branch record) never had its index register
  // identified by a dedicated detector, leaving Info.IndexReg unset — which
  // disables every index-keyed bound strategy (mask / modulo / range-guard).
  // Anchoring on the table load recovers it here so those strategies engage.
  for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I) {
    auto &L = BlockOps[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    uint64_t BaseReg = InvalidVA, IdxReg = InvalidVA, LoadDisp = 0;
    bool Scaled = false;
    if (!analyzeTableLoadAddr(BlockOps, I - 1, AddrV, BaseReg, IdxReg, Scaled,
                              LoadDisp)) {
      // analyzeTableLoadAddr requires a *register* base (`base + index*scale`).
      // A non-PIC absolute table folds the base to a constant in the dispatch
      // (`jmp *tab(,idx,8)` => addr = INT_ADD(index*scale, const)), so it is
      // rejected there.  Recover the scaled-index register directly from the
      // load-address INT_ADD instead — either operand may be the scaled index,
      // the other the constant base — so the index-keyed bound strategies still
      // engage for constant-base tables.
      int AddIdx = reachingDefIdx(BlockOps, I - 1, AddrV);
      for (int G = 0;
           AddIdx >= 0 && BlockOps[AddIdx].Opcode == NdOp::COPY &&
           BlockOps[AddIdx].NumInputs >= 1 && G < limits::kMaxQuasiCopyDepth;
           ++G)
        AddIdx =
            reachingDefIdx(BlockOps, AddIdx - 1, BlockOps[AddIdx].Inputs[0]);
      if (AddIdx < 0 || BlockOps[AddIdx].Opcode != NdOp::INT_ADD ||
          BlockOps[AddIdx].NumInputs < 2)
        continue;
      for (int W = 0; W < 2; ++W) {
        uint64_t Idx =
            scaledIndexReg(BlockOps, AddIdx - 1, BlockOps[AddIdx].Inputs[W]);
        if (Idx != InvalidVA) {
          IdxReg = Idx;
          break;
        }
      }
      if (IdxReg == InvalidVA)
        continue;
    }
    if (Info.IndexReg == InvalidVA)
      Info.IndexReg = IdxReg;
    traceIndexTransform(BlockOps, I - 1, NdVar::reg(IdxReg, 8), Info.NormBase,
                        Info.NormShift, Info.Stride);
    return;
  }

  // Fallback: when the index/table load could not be resolved, scan only the
  // INDIR_BR record's own ops for a local normalization (low-risk, no
  // cross-instruction guessing).
  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;
    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst()) {
        int64_t Base = static_cast<int64_t>(Op.Inputs[1].Offset);
        if (Base > 0 && Base <= limits::kMaxNormBase)
          Info.NormBase = Base;
      }
      if ((Op.Opcode == NdOp::INT_RIGHT || Op.Opcode == NdOp::INT_ASHR) &&
          Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
        uint32_t Shift = static_cast<uint32_t>(Op.Inputs[1].Offset);
        if (Shift > 0 && Shift <= limits::kMaxNormShift)
          Info.NormShift = Shift;
      }
    }
    break;
  }
}

//===----------------------------------------------------------------------===//
// detectStride — infer switch variable stride from AND masks
//===----------------------------------------------------------------------===//

/// Detect a stride (power-of-2 alignment) on the switch variable by
/// looking for INT_AND with a mask that has known-zero low bits.  When
/// the guard bound is N and the stride is S, the effective table size
/// is N / S.
void CFGBuilder::detectStride(const InsnRecord &Rec, JumpTableInfo &Info) {
  uint64_t SwitchReg = InvalidVA;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
        Op.Inputs[0].isReg()) {
      SwitchReg = Op.Inputs[0].Offset;
      break;
    }
  }
  if (SwitchReg == InvalidVA)
    return;

  uint64_t SwitchSrc =
      quasiCopySource(Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, SwitchReg);

  // Scan the INDIR_BR instruction's ops for an AND mask.
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode != NdOp::INT_AND || Op.NumInputs < 2 ||
        !Op.Inputs[1].isConst())
      continue;
    if (!Op.Inputs[0].isReg())
      continue;

    uint64_t GuardSrc = quasiCopySource(
        Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, Op.Inputs[0].Offset);
    if (GuardSrc != SwitchSrc)
      continue;

    uint64_t Mask = Op.Inputs[1].Offset;
    if (Mask == 0)
      continue;

    uint32_t Stride = 1;
    uint64_t M = Mask;
    while ((M & 1) == 0 && Stride < limits::kMaxStrideScanBits) {
      M >>= 1;
      Stride <<= 1;
    }

    if (Stride > 1 && Stride <= limits::kMaxEntryBytes) {
      Info.Stride = Stride;
      LLVM_DEBUG(llvm::dbgs()
                 << "  stride: detected stride=" << Stride
                 << " from AND mask 0x" << llvm::utohexstr(Mask) << "\n");
    }
    break;
  }

  // A cross-instruction stride (a mask applied to the index in an earlier
  // instruction of the block) is recovered by detectNormalization's index-chain
  // trace, which is confined to the dataflow that feeds the table load.  An
  // unconstrained block scan here would instead latch onto any masked value
  // (e.g. a case body's `and x,6`) and report a phantom stride, so it is
  // intentionally omitted.
}

} // namespace neverd
