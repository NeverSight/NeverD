//===- JumpTableResolverSource.cpp - Table base-address detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Table base-address ("source") detection strategies for jump-table
/// resolution: recover where a table's entries live in memory when the base is
/// materialized across instructions -- PIC/relative bases set in a prior insn,
/// stack-materialized bases, and runtime-selected two-table bases.  All
/// strategies here are architecture-neutral pattern matchers.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// the top-level dispatch and JumpTableResolverDetail.h for the shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/libc/LibCNames.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

uint64_t
CFGBuilder::forwardIndexThroughStackSpill(const std::vector<LowOp> &BlockOps,
                                          int LoadIdx, uint64_t IndexReg,
                                          va_t BlkStart) const {
  if (IndexReg == InvalidVA || LoadIdx <= 0 || !CurrentImg)
    return IndexReg;
  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);

  // Trace the index register's reaching def (before the table load) to an
  // underlying LOAD, through a couple of COPY/extend hops — the -O0 reload of a
  // spilled switch variable (`ldr rIdx,[sp,#k]`).
  NdVar Cur = NdVar::reg(IndexReg, 4);
  int From = LoadIdx - 1;
  int LdIdx = -1;
  for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
    int D = reachingDefIdx(BlockOps, From, Cur);
    if (D < 0)
      return IndexReg;
    const LowOp &O = BlockOps[D];
    if (O.Opcode == NdOp::LOAD) {
      LdIdx = D;
      break;
    }
    if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
         O.Opcode == NdOp::INT_SEXT) &&
        O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
      Cur = O.Inputs[0];
      From = D - 1;
      continue;
    }
    return IndexReg;
  }
  if (LdIdx < 0)
    return IndexReg;

  // The reload address must be a plain SP/FP slot; capture its (base, offset).
  const LowOp &Ld = BlockOps[LdIdx];
  NdVar AddrV = (Ld.NumInputs >= 2) ? Ld.Inputs[1] : Ld.Inputs[0];
  uint64_t SlotBase = InvalidVA;
  int64_t SlotOff = 0;
  if (!frameSlotKey(BlockOps, LdIdx - 1, AddrV, TRI, SlotBase, SlotOff))
    return IndexReg;

  // The value stored to the slot is often a *copy* of the guarded switch
  // variable (`and eax,7; mov ecx,eax; mov [slot],ecx`), and that intermediate
  // register (ecx) is frequently reused as the table base at the dispatch
  // (`lea rcx,[rip]; mov (rcx,idx,4)`).  Forwarding the index to that clobbered
  // copy loses both the range guard and the index mask (which live on the
  // original eax), so the bound defaults to the raw relocation run and a table
  // adjacent in rodata is over-read.  Trace the stored value to its ultimate
  // copy source instead: guardConstrainsIndex traces a guard operand back to
  // its own source, so a guard on any register in the copy chain still matches
  // the earliest source, and that source is not the reused base.
  auto ultimateSource = [&](uint64_t Reg, va_t StoreAddr) -> uint64_t {
    std::vector<LowOp> Pre;
    for (auto PIt = Insns.lower_bound(CurrentFuncEntry);
         PIt != Insns.end() && PIt->first <= StoreAddr; ++PIt)
      for (auto &PO : PIt->second.Ops)
        Pre.push_back(PO);
    uint64_t Ult = traceRegSource(Pre, static_cast<int>(Pre.size()) - 1, Reg);
    return (Ult != InvalidVA) ? Ult : Reg;
  };

  // A store to the same slot earlier in the dispatch block takes precedence
  // over any predecessor store (it is the value the reload actually observes).
  for (int I = LdIdx - 1; I >= 0; --I) {
    const LowOp &Op = BlockOps[I];
    if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
      continue;
    uint64_t B = InvalidVA;
    int64_t Off = 0;
    if (!frameSlotKey(BlockOps, I - 1, Op.Inputs[0], TRI, B, Off))
      continue;
    if (B != SlotBase || Off != SlotOff)
      continue;
    uint64_t Src = traceToRegister(BlockOps, I - 1, Op.Inputs[1]);
    if (Src == InvalidVA)
      return IndexReg;
    return ultimateSource(Src, BlockOps[I].Addr);
  }

  // Otherwise the spill is in a predecessor — the nearest STORE to the same
  // slot before the dispatch block is the guarded switch variable.
  for (auto It = Insns.lower_bound(BlkStart); It != Insns.begin();) {
    --It;
    const InsnRecord &IR = It->second;
    for (int I = static_cast<int>(IR.Ops.size()) - 1; I >= 0; --I) {
      const LowOp &Op = IR.Ops[I];
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
        continue;
      uint64_t B = InvalidVA;
      int64_t Off = 0;
      if (!frameSlotKey(IR.Ops, I - 1, Op.Inputs[0], TRI, B, Off))
        continue;
      if (B != SlotBase || Off != SlotOff)
        continue;
      uint64_t Src = traceToRegister(IR.Ops, I - 1, Op.Inputs[1]);
      if (Src == InvalidVA)
        return IndexReg;
      return ultimateSource(Src, It->first);
    }
  }
  return IndexReg;
}

va_t CFGBuilder::resolveStackMaterializedTableSource(
    const BinaryImage &Img, const InsnRecord &Rec,
    const std::vector<LowOp> &Ops, int LoadIdx, uint64_t BaseReg,
    uint16_t LoadWidth, int64_t TableDisp, bool *MutatedOut) const {
  if (BaseReg == InvalidVA || LoadIdx < 0 || LoadWidth == 0 || !CurrentImg)
    return InvalidVA;
  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);

  // Flatten the function prefix up to the dispatch first: the init store and
  // its source LOAD live in the entry block (not the single-predecessor
  // dispatch path), and at -O2 the table-base register itself (`x10 = sp`) is
  // defined in the entry block while the LOAD/branch sit in the dispatch block,
  // so the base must be traceable over the whole prefix too.
  std::vector<LowOp> FuncOps;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      FuncOps.push_back(Op);

  // Resolve a value (a register holding a computed address, or a load/store
  // address temp) to a frame slot (frame register + signed byte offset) by
  // tracing COPY / INT_ADD(const) / INT_SUB(const).  Unlike the shared
  // frameSlotKey this *follows stack-pointer redefinitions*: a pre-indexed
  // store/alloc (`stp q0,q1,[sp,#-0x30]!`, common at -O2) mutates SP mid
  // function, so an init store written relative to the entry SP and the table
  // base taken from the post-update SP must be canonicalised to the same entry
  // frame register or their offsets would not line up.  The frame pointer is
  // stable (set once), so only SP is followed, and only through plain frame
  // arithmetic — a non-followable SP def (`and sp,sp,#-16` realignment,
  // `sub sp,sp,xN` VLA) falls back to treating that SP as the canonical base,
  // matching the non-mutating model (no behaviour change for those shapes).
  auto canonFrameSlot = [&](const std::vector<LowOp> &O, NdVar Start,
                            int StartFrom, uint64_t &OutReg, int64_t &OutOff,
                            bool FollowSubpiece = false) -> bool {
    int64_t Off = 0;
    NdVar Cur = Start;
    int From = StartFrom;
    bool HaveFallback = false;
    uint64_t FbReg = 0;
    int64_t FbOff = 0;
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      if (Cur.isReg() && TRI.isFrameReg(Cur.Offset)) {
        if (!TRI.isStackPointer(
                Cur.Offset)) { // frame pointer: stable, canonical
          OutReg = Cur.Offset;
          OutOff = Off;
          return true;
        }
        HaveFallback = true; // SP: record, then try to follow a mutation
        FbReg = Cur.Offset;
        FbOff = Off;
      }
      int D = (Cur.isReg() || Cur.isTemp()) ? reachingDefIdx(O, From, Cur) : -1;
      if (D < 0) {
        if (HaveFallback) {
          OutReg = FbReg;
          OutOff = FbOff;
          return true;
        }
        return false;
      }
      const LowOp &Op = O[D];
      if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      // Low-half extraction of a wider temp: on 32-bit targets the lifter
      // models a `lea`/address computation in a 64-bit temp and narrows it to
      // the 32-bit register with `SUBBYTES(x, 0)`.  The low half holds the
      // (32-bit) address, so follow it like a rename to keep tracing the frame
      // arithmetic behind it (a frame-slot value passed as a memcpy argument
      // reaches canonFrameSlot through this narrow).  Opt-in only: the
      // table-base / init-store-address traces never cross a SUBBYTES in
      // practice, and unconditionally following it there mis-resolves some
      // 32-bit stack jump tables, so this is enabled solely for the memcpy
      // destination-argument recovery that needs it.
      if (FollowSubpiece && Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
        int CW = Op.Inputs[1].isConst() ? 1 : (Op.Inputs[0].isConst() ? 0 : -1);
        if (CW >= 0 &&
            scaledIndexReg(O, D - 1, Op.Inputs[1 - CW]) == InvalidVA) {
          Off += static_cast<int64_t>(Op.Inputs[CW].Offset);
          Cur = Op.Inputs[1 - CW];
          From = D - 1;
          continue;
        }
        // base + scaled-index: ARM/Thumb -O0 folds the scaled table index into
        // the base register itself (`add r0,sp,#k; add r0,r0,idx,lsl#2;
        // ldr [r0]`), so the frame-slot base is reused and the table-base trace
        // reaches this add before the pure base.  The index is not part of the
        // frame slot, so follow the non-index (base) operand and drop it.
        int SI = (scaledIndexReg(O, D - 1, Op.Inputs[1]) != InvalidVA)   ? 1
                 : (scaledIndexReg(O, D - 1, Op.Inputs[0]) != InvalidVA) ? 0
                                                                         : -1;
        if (SI >= 0 &&
            (Op.Inputs[1 - SI].isReg() || Op.Inputs[1 - SI].isTemp())) {
          Cur = Op.Inputs[1 - SI];
          From = D - 1;
          continue;
        }
        if (HaveFallback) {
          OutReg = FbReg;
          OutOff = FbOff;
          return true;
        }
        return false;
      }
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst()) {
        Off -= static_cast<int64_t>(Op.Inputs[1].Offset);
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      // Unfollowable SP def: fall back to the recorded SP as the canonical
      // base.
      if (HaveFallback) {
        OutReg = FbReg;
        OutOff = FbOff;
        return true;
      }
      return false;
    }
    if (HaveFallback) {
      OutReg = FbReg;
      OutOff = FbOff;
      return true;
    }
    return false;
  };

  // 1) The table-base register must resolve to a frame slot.  Trace it over the
  //    function prefix (FuncOps) anchored at the table LOAD's position: the
  //    base register is a reused scratch register, so its reaching def must be
  //    the one live *at the load* (not the latest in the function), and FuncOps
  //    — unlike the passed dispatch/path ops — includes the entry-block
  //    prologue `sub sp,sp,#N`, so the stack-pointer adjustment is followed
  //    consistently with the init-store scan (step 3, which always uses
  //    FuncOps).  Locate the load in FuncOps by its instruction address +
  //    output nd-var.
  int LoadPosInFunc = -1;
  if (LoadIdx >= 0 && LoadIdx < static_cast<int>(Ops.size())) {
    va_t LdAddr = Ops[LoadIdx].Addr;
    const NdVar &LdOut = Ops[LoadIdx].Output;
    for (int I = 0; I < static_cast<int>(FuncOps.size()); ++I)
      if (FuncOps[I].Opcode == NdOp::LOAD && FuncOps[I].Addr == LdAddr &&
          FuncOps[I].Output.Offset == LdOut.Offset &&
          FuncOps[I].Output.Size == LdOut.Size &&
          FuncOps[I].Output.isTemp() == LdOut.isTemp())
        LoadPosInFunc = I; // last match at that address wins
  }
  int BaseFrom = (LoadPosInFunc >= 0) ? LoadPosInFunc - 1
                                      : static_cast<int>(FuncOps.size()) - 1;
  uint64_t FrameReg = InvalidVA;
  int64_t FrameOff = 0;
  if (!canonFrameSlot(FuncOps, NdVar::reg(BaseReg, 8), BaseFrom, FrameReg,
                      FrameOff))
    return InvalidVA;
  // x86-64/i386 -O0 ride the table's frame offset in the load displacement
  // (`mov (%rbp,%idx,8),-0x30`), not in the base register (AArch64
  // `add xB,sp,#k`); fold it into the resolved frame slot so the init-store
  // scan below matches the slot the initializer run was copied into.
  FrameOff += TableDisp;

  // Trace a stored value back to the constant (read-only) VA it was ultimately
  // loaded from, following value-preserving COPY chains to the defining LOAD.
  // Two materialisation shapes reach the same __const initializer run:
  //   * Direct: `ldr q0,[__const]; str q0,[slot]` — the LOAD address folds to a
  //     constant data VA (foldRegConstant).
  //   * Staging buffer (clang -O0 for >=5-entry local tables): the initializer
  //     run is first copied to a scratch frame area, the real table is
  //     `memset`-cleared, then the scratch is reloaded and re-stored
  //     (`ldr x,[sp+k]; ... stur x,[fp-j]`).  Here the LOAD reads a *frame
  //     slot*, so recurse through the store that filled that scratch slot to
  //     reach the
  //     __const LOAD.
  // Returns the const VA corresponding to the *start* of `Val` (callers add any
  // intra-store entry offset).  The code-pointer reloc-run gate at the call
  // sites keeps this from misreading a non-table register-indirect branch.
  std::function<std::optional<va_t>(NdVar, int, int)> traceValToConstSrc =
      [&](NdVar Val, int VFrom, int Depth) -> std::optional<va_t> {
    if (Depth > 4)
      return std::nullopt;
    int LdD = -1;
    {
      NdVar V = Val;
      int From = VFrom;
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!V.isReg() && !V.isTemp())
          break;
        int D = reachingDefIdx(FuncOps, From, V);
        if (D < 0)
          break;
        const LowOp &O = FuncOps[D];
        if (O.Opcode == NdOp::LOAD) {
          LdD = D;
          break;
        }
        // NEON vector-lane materialisation (clang -O2 for >=5-entry local
        // tables): the table run is assembled into a 128-bit register lane by
        // lane (`fmov d0,x; mov.d v0[1],x; stp q0,q1,[slot]`) rather than
        // copied with a clean SIMD load/store.  CONCAT(hi, lo) is the lane
        // assembly; recurse into both halves and require their const sources to
        // be contiguous (hi == lo + lo.size) — i.e. the vector faithfully
        // mirrors a const sub-run — then return the run start (lo).  A
        // shuffled/permuted assembly is not contiguous and is rejected (the
        // caller traps).
        if (O.Opcode == NdOp::CONCAT && O.NumInputs >= 2) {
          auto Lo = traceValToConstSrc(O.Inputs[1], D - 1, Depth + 1);
          auto Hi = traceValToConstSrc(O.Inputs[0], D - 1, Depth + 1);
          if (Lo && Hi &&
              static_cast<int64_t>(*Hi) ==
                  static_cast<int64_t>(*Lo) +
                      static_cast<int64_t>(O.Inputs[1].Size))
            return Lo;
          return std::nullopt;
        }
        if (O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
            (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        // A lane scalar is widened to the q-register (INT_ZEXT d->q) and its
        // low element re-extracted (SUBBYTES v,0) during the vector assembly
        // above; both preserve the low bytes' const source, so follow them to
        // the defining LOAD.
        if ((O.Opcode == NdOp::INT_ZEXT ||
             (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
              O.Inputs[1].isConst() && O.Inputs[1].Offset == 0)) &&
            O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        break;
      }
    }
    if (LdD < 0)
      return std::nullopt;
    const LowOp &Ld = FuncOps[LdD];
    NdVar AddrV = (Ld.NumInputs >= 2) ? Ld.Inputs[1] : Ld.Inputs[0];

    // Staging buffer: the value came from a frame slot an earlier store filled.
    // Resolve that slot and recurse through the latest covering store's value.
    uint64_t SlotReg = InvalidVA;
    int64_t SlotOff = 0;
    if (frameSlotKey(FuncOps, LdD - 1, AddrV, TRI, SlotReg, SlotOff)) {
      int64_t LdW = static_cast<int64_t>(Ld.Output.Size);
      for (int I = LdD - 1; I >= 0; --I) {
        const LowOp &St = FuncOps[I];
        if (St.Opcode != NdOp::STORE || St.NumInputs < 2)
          continue;
        uint64_t SB = InvalidVA;
        int64_t SOff = 0;
        if (!frameSlotKey(FuncOps, I - 1, St.Inputs[0], TRI, SB, SOff))
          continue;
        if (SB != SlotReg)
          continue;
        int64_t SS = static_cast<int64_t>(St.Inputs[1].Size);
        if (SS <= 0 || SOff > SlotOff || SlotOff + LdW > SOff + SS)
          continue; // store does not cover the loaded slice
        auto Src = traceValToConstSrc(St.Inputs[1], I - 1, Depth + 1);
        if (!Src)
          return std::nullopt;
        return static_cast<va_t>(static_cast<int64_t>(*Src) + (SlotOff - SOff));
      }
      return std::nullopt;
    }

    // Constant data source: decompose the LOAD address into base reg + const
    // displacement, then fold the base register at the load to a const data VA.
    uint64_t AddrReg = InvalidVA;
    int64_t AddrDisp = 0;
    bool ConstBase = false;
    uint64_t ConstBaseVA = 0;
    {
      NdVar A = AddrV;
      int AFrom = LdD - 1;
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (A.isReg()) {
          AddrReg = A.Offset;
          break;
        }
        // x86-64/i386 RIP/PC-relative table-entry load: the lifter folds the
        // PC-relative address to a constant base (`COPY tmp,<RIP>; INT_ADD
        // tmp,disp`), so the chain terminates at a constant rather than a base
        // register.  The absolute __const VA is that constant plus the
        // accumulated displacement (AArch64 keeps an adrp/add register base, so
        // this branch is x86-only).
        if (A.isConst()) {
          ConstBase = true;
          ConstBaseVA = A.Offset;
          break;
        }
        if (!A.isTemp())
          break;
        int D = reachingDefIdx(FuncOps, AFrom, A);
        if (D < 0)
          break;
        const LowOp &O = FuncOps[D];
        if (O.Opcode == NdOp::COPY && O.NumInputs >= 1) {
          A = O.Inputs[0];
          AFrom = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
          int CW = O.Inputs[1].isConst() ? 1 : (O.Inputs[0].isConst() ? 0 : -1);
          if (CW < 0)
            break;
          AddrDisp += static_cast<int64_t>(O.Inputs[CW].Offset);
          A = O.Inputs[1 - CW];
          AFrom = D - 1;
          continue;
        }
        break;
      }
    }
    if (ConstBase)
      return static_cast<va_t>(static_cast<int64_t>(ConstBaseVA) + AddrDisp);
    if (AddrReg == InvalidVA)
      return std::nullopt;
    std::optional<uint64_t> Folded =
        foldRegConstant(Img, Rec, AddrReg, Ld.Addr);
    if (Folded && *Folded != 0)
      return static_cast<va_t>(static_cast<int64_t>(*Folded) + AddrDisp);
    // i386 PIC GOTOFF init: the base register is the GOT base, which equals the
    // image base (0) in the relocatable model, so foldRegConstant (which
    // rejects a 0 fold) cannot resolve it.  Fall back to GOT-base-0 — the table
    // source is the GOTOFF displacement itself.  Sound because the only caller
    // gates every recovered source on an absolute code-pointer reloc run, so a
    // base whose fold merely failed (no GOTOFF table behind it) is rejected
    // there.
    if (AddrDisp != 0)
      return static_cast<va_t>(AddrDisp);
    return std::nullopt;
  };

  // 3) Find the STORE that initialised the table's frame slot from a constant
  //    data source.  clang copies the read-only initializer run (one scalar or
  //    SIMD store per 1-2 entries), so a store whose byte span covers the table
  //    base offset and whose value traces (via traceValToConstSrc, including
  //    through a staging buffer) to a __const LOAD pins the source.  Prefer the
  //    latest such store before dispatch.
  va_t BestSource = InvalidVA;
  for (int I = 0; I < static_cast<int>(FuncOps.size()); ++I) {
    const LowOp &Op = FuncOps[I];
    if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
      continue;
    uint64_t B = InvalidVA;
    int64_t Off = 0;
    if (!canonFrameSlot(FuncOps, Op.Inputs[0], I - 1, B, Off))
      continue;
    if (B != FrameReg)
      continue;
    int64_t StoreSize = static_cast<int64_t>(Op.Inputs[1].Size);
    if (StoreSize <= 0 || Off > FrameOff || FrameOff >= Off + StoreSize)
      continue;
    int64_t EntryDelta = FrameOff - Off; // table base within the stored value

    // Trace the stored value (directly or through a staging buffer) to the
    // __const VA it was loaded from; the table base sits EntryDelta into it.
    auto SrcOpt = traceValToConstSrc(Op.Inputs[1], I - 1, 0);
    if (!SrcOpt)
      continue;
    va_t Source = static_cast<va_t>(static_cast<int64_t>(*SrcOpt) + EntryDelta);

    // The source must be a constant (read-only) data region carrying a run of
    // absolute code-pointer relocations — the verifiable signature of a label
    // table (the initializer run lives in __DATA_CONST / .data.rel.ro, which
    // the loader may flag writable, so the reloc run, not the segment
    // permission, is the gate).  This is what distinguishes a
    // stack-materialised computed-goto table from any other stack array, so a
    // non-table register-indirect branch is never misread.
    const auto *Seg = Img.getSegmentFor(Source);
    if (!Seg || Seg->Data.empty())
      continue;
    if (countCodePtrRelocRun(Img, Source, LoadWidth) <
        limits::kMinJumpTableEntries)
      continue;
    BestSource = Source; // keep scanning; latest valid init wins
  }

  // Larger local tables (clang -O0, ~>=8 entries) are not copied store-by-store
  // but with a single `memcpy(table_slot, __const_run, size)` call, so the
  // store scan above finds nothing.  Recognise that init: a memcpy/memmove
  // whose destination (first integer argument) is the table's frame slot and
  // whose source (second argument) folds to a __const VA carrying a
  // code-pointer reloc run.  The reloc-run gate is the table signature, so a
  // non-table memcpy into a stack buffer is never misread.
  if (BestSource == InvalidVA && TRI.IntParamRegs.size() >= 2) {
    for (int I = 0; I < static_cast<int>(FuncOps.size()); ++I) {
      const LowOp &Op = FuncOps[I];
      if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
          !Op.Inputs[0].isConst())
        continue;
      va_t Target = static_cast<va_t>(Op.Inputs[0].Offset);
      bool IsCopy = false;
      if (const Import *Imp = Img.findImportAt(Target))
        IsCopy = libc::isMemCopyName(Imp->Name);
      if (!IsCopy)
        if (const Symbol *Sym = Img.findSymbolAt(Target))
          IsCopy = libc::isMemCopyName(Sym->Name);
      // Relocatable objects (the lift roundtrip input) leave the call's
      // constant target at the placeholder 0 — the real callee is named by a
      // relocation at the call instruction (x86 `call rel32` reloc at +1,
      // AArch64/ARM `bl` reloc at +0).  When the resolved constant misses,
      // consult the call-site relocation's symbol name so a memcpy/memmove
      // table-init copy is still recognised.  The code-pointer reloc-run gate
      // below remains the table signature, so a non-table memcpy is never
      // misread as a jump table.
      if (!IsCopy)
        for (const RelocationEntry &R : Img.Relocations)
          if ((R.Address == Op.Addr || R.Address == Op.Addr + 1) &&
              !R.SymbolName.empty() && libc::isMemCopyName(R.SymbolName)) {
            IsCopy = true;
            break;
          }
      if (!IsCopy)
        continue;
      // arg0 (dst) must resolve to the table's frame slot, covering the base.
      // The memcpy destination is a register holding a computed frame address
      // (`add x0,sp,#k`); canonFrameSlot traces it the same way as the table
      // base so both share one stack-pointer-aware canonicalisation.
      uint64_t DB = InvalidVA;
      int64_t DOff = 0;
      // arg0 (dst, the table frame slot) and arg1 (src, the const initializer
      // VA).  Register-based ABIs (x86-64 SysV / AArch64 / ARM) pass them in
      // IntParamRegs[0]/[1]; i386 cdecl passes them on the outgoing stack
      // ([sp+0]=dst, [sp+ptr]=src), recovered from the stores that fill those
      // slots just before the call.
      // Resolve a value that holds a constant data address (the memcpy source =
      // address-of the initializer run) to its absolute VA: strip COPY /
      // low-half SUBBYTES renames, sum constant addends, then fold the base
      // register — falling back to the GOTOFF displacement with GOT-base-0 when
      // the i386 get_pc_thunk base cannot be emulated (its `call;pop;add` halts
      // the folder), mirroring traceValToConstSrc's load-address handling.  The
      // reloc-run gate below keeps this sound.
      auto addrToConstVA = [&](NdVar A,
                               int AFrom) -> std::optional<uint64_t> {
        int64_t Disp = 0;
        uint64_t Reg = InvalidVA;
        bool ConstBase = false;
        uint64_t ConstVA = 0;
        for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
          if (A.isConst()) {
            ConstBase = true;
            ConstVA = A.Offset;
            break;
          }
          if (!A.isReg() && !A.isTemp())
            break;
          int D = reachingDefIdx(FuncOps, AFrom, A);
          if (D >= 0) {
            const LowOp &O = FuncOps[D];
            // COPY / low-half SUBBYTES are pure renames — follow for both regs
            // and temps (the i386 `lea` result reaches the stored arg through a
            // 64-bit temp narrowed to the 32-bit register).
            if ((O.Opcode == NdOp::COPY ||
                 (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
                  O.Inputs[1].isConst() && O.Inputs[1].Offset == 0)) &&
                O.NumInputs >= 1 &&
                (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
              A = O.Inputs[0];
              AFrom = D - 1;
              continue;
            }
            // Sum a constant addend only while the running base is a TEMP: a
            // register base is the terminal (e.g. the i386 GOT base register,
            // whose get_pc_thunk arithmetic must not be folded into the GOTOFF
            // displacement).
            if (A.isTemp() && O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
              int CW =
                  O.Inputs[1].isConst() ? 1 : (O.Inputs[0].isConst() ? 0 : -1);
              if (CW >= 0 &&
                  (O.Inputs[1 - CW].isReg() || O.Inputs[1 - CW].isTemp())) {
                Disp += static_cast<int64_t>(O.Inputs[CW].Offset);
                A = O.Inputs[1 - CW];
                AFrom = D - 1;
                continue;
              }
            }
          }
          if (A.isReg())
            Reg = A.Offset;
          break;
        }
        if (ConstBase)
          return static_cast<uint64_t>(static_cast<int64_t>(ConstVA) + Disp);
        if (Reg != InvalidVA) {
          if (auto F = foldRegConstant(Img, Rec, Reg, Op.Addr); F && *F != 0)
            return static_cast<uint64_t>(static_cast<int64_t>(*F) + Disp);
          if (Disp != 0) // i386 GOTOFF init: GOT base == image base (0)
            return static_cast<uint64_t>(Disp);
        }
        return std::nullopt;
      };

      std::optional<uint64_t> Folded;
      bool ArgsOk = false;
      // Register-passed args (x86-64 SysV / AArch64 / ARM): dst in
      // IntParamRegs[0], src in IntParamRegs[1].  Commit only when the source
      // also folds — on i386 the dst register may coincidentally still hold the
      // table address while the args truly live on the stack, so a register dst
      // with no register source must fall through to the stack recovery below.
      if (canonFrameSlot(FuncOps, NdVar::reg(TRI.IntParamRegs[0], 8), I - 1,
                         DB, DOff, /*FollowSubpiece=*/true)) {
        if (auto F = foldRegConstant(Img, Rec, TRI.IntParamRegs[1], Op.Addr);
            F && *F != 0) {
          Folded = F;
          ArgsOk = true;
        }
      }
      // Stack-passed args (i386 cdecl): dst=[sp+0], src=[sp+ptr], recovered
      // from the stores that fill the outgoing argument slots just before the
      // call.
      if (!ArgsOk) {
        NdVar DstVal, SrcVal;
        int DstFrom = -1, SrcFrom = -1;
        bool HaveDst = false, HaveSrc = false;
        int64_t PtrSz = static_cast<int64_t>(Img.getPointerSize());
        for (int J = I - 1; J >= 0 && !(HaveDst && HaveSrc); --J) {
          const LowOp &St = FuncOps[J];
          if (St.Opcode != NdOp::STORE || St.NumInputs < 2)
            continue;
          uint64_t SB = InvalidVA;
          int64_t SOff = 0;
          if (!frameSlotKey(FuncOps, J - 1, St.Inputs[0], TRI, SB, SOff))
            continue;
          if (!TRI.isStackPointer(SB))
            continue;
          if (SOff == 0 && !HaveDst) {
            DstVal = St.Inputs[1];
            DstFrom = J - 1;
            HaveDst = true;
          } else if (SOff == PtrSz && !HaveSrc) {
            SrcVal = St.Inputs[1];
            SrcFrom = J - 1;
            HaveSrc = true;
          }
        }
        if (HaveDst && HaveSrc &&
            canonFrameSlot(FuncOps, DstVal, DstFrom, DB, DOff,
                           /*FollowSubpiece=*/true)) {
          Folded = addrToConstVA(SrcVal, SrcFrom);
          ArgsOk = Folded.has_value();
        }
      }
      if (!ArgsOk)
        continue;
      if (DB != FrameReg || DOff > FrameOff)
        continue;
      int64_t EntryDelta = FrameOff - DOff;
      if (!Folded || *Folded == 0)
        continue;
      va_t Source =
          static_cast<va_t>(static_cast<int64_t>(*Folded) + EntryDelta);
      const auto *Seg = Img.getSegmentFor(Source);
      if (!Seg || Seg->Data.empty())
        continue;
      if (countCodePtrRelocRun(Img, Source, LoadWidth) <
          limits::kMinJumpTableEntries)
        continue;
      BestSource = Source; // latest valid copy wins
    }
  }

  // Soundness gate: an index-dispatch switch over the recovered static targets
  // is only correct if the stack table still holds the *positional* constant
  // entries at run time.  When the program overwrites an entry slot after the
  // initializer with a value that is not the constant entry for that slot — a
  // runtime permutation (`void *t=tab[0]; tab[0]=tab[3]; tab[3]=t;`) — the
  // static map no longer describes the runtime index->target mapping.  Flag it
  // so the emitter traps loudly instead of silently selecting the wrong case
  // (sound resolution would need runtime value dispatch — a documented gap).
  if (BestSource != InvalidVA && MutatedOut) {
    // Const VA the value stored by FuncOps[StoreIdx] is copied from, or nullopt
    // when it does not trace to a foldable constant load.  Reuses the shared
    // initializer trace (which also follows a staging buffer), so a faithful
    // staged init store is recognised as constant, not flagged as a
    // permutation.
    auto constSrcOfStoreValue = [&](int StoreIdx) -> std::optional<va_t> {
      return traceValToConstSrc(FuncOps[StoreIdx].Inputs[1], StoreIdx - 1, 0);
    };

    uint32_t N = countCodePtrRelocRun(Img, BestSource, LoadWidth);
    int64_t RegionLo = FrameOff;
    int64_t RegionHi = FrameOff + static_cast<int64_t>(N) * LoadWidth;

    // A store whose address is `table_base + variable_index` (the same shape as
    // the dispatch load, e.g. `tab[k] = ...` with a non-constant k) can write a
    // non-positional value into any entry, which canonFrameSlot rejects (the
    // scaled index has no constant offset).  Such a write cannot be proven
    // faithful, so it must flag the table as mutated rather than be skipped —
    // otherwise a runtime-permuted table is dispatched on its stale static map.
    auto storeHitsTableVarIndex = [&](NdVar AddrV, int FromIdx) -> bool {
      NdVar A = AddrV;
      int From = FromIdx;
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!A.isReg() && !A.isTemp())
          return false;
        int D = reachingDefIdx(FuncOps, From, A);
        if (D < 0)
          return false;
        const LowOp &O = FuncOps[D];
        if (O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
            (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          A = O.Inputs[0];
          From = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
          for (int S = 0; S < 2; ++S) {
            if (scaledIndexReg(FuncOps, D - 1, O.Inputs[S]) == InvalidVA)
              continue;
            uint64_t BB = InvalidVA;
            int64_t BOff = 0;
            if (canonFrameSlot(FuncOps, O.Inputs[1 - S], D - 1, BB, BOff) &&
                BB == FrameReg && BOff >= RegionLo && BOff < RegionHi)
              return true; // variable-index write into the table region
          }
        }
        return false;
      }
      return false;
    };

    for (int I = 0; I < static_cast<int>(FuncOps.size()); ++I) {
      const LowOp &Op = FuncOps[I];
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
        continue;
      uint64_t B = InvalidVA;
      int64_t Off = 0;
      if (!canonFrameSlot(FuncOps, Op.Inputs[0], I - 1, B, Off)) {
        if (storeHitsTableVarIndex(Op.Inputs[0], I - 1)) {
          *MutatedOut = true;
          break;
        }
        continue;
      }
      if (B != FrameReg)
        continue;
      int64_t SS = static_cast<int64_t>(Op.Inputs[1].Size);
      if (SS <= 0 || Off + SS <= RegionLo || Off >= RegionHi)
        continue; // store does not touch the table's frame region
      // A faithful initializer store copies constant entry e to slot e: its
      // base offset is entry-aligned and its value is loaded from BestSource +
      // (Off - base).  Anything else — a permuting overwrite or a non-constant
      // value — breaks the positional map, so the static targets are unsound.
      bool Faithful = false;
      if (Off >= RegionLo && (Off - RegionLo) % LoadWidth == 0) {
        if (auto Src = constSrcOfStoreValue(I))
          Faithful =
              (*Src == static_cast<va_t>(static_cast<int64_t>(BestSource) +
                                         (Off - RegionLo)));
      }
      if (!Faithful) {
        *MutatedOut = true;
        break;
      }
    }
  }

  return BestSource;
}

// A pre-scaled computed-goto index is `(value >> s) & M`, where the mask M is a
// contiguous bit run shifted left by k (`(2^m - 1) << k`) and 2^k equals the
// pointer-width entry size: the `& M` confines the result to the byte offsets
// {0, size, 2*size, ...} of an `size`-byte entry table.  Returns true when
// `V` (traced through COPY) is defined by exactly that mask op.
static bool isPreScaledIndex(const std::vector<LowOp> &Ops, int FromIdx,
                             NdVar V, uint16_t EntrySize) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (!V.isReg() && !V.isTemp())
      return false;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return false;
    const LowOp &Op = Ops[D];
    // The masked index is routinely zero-extended to pointer width for the load
    // address (x86-64 `and esi,0x38; <zext esi>`), so follow value-preserving
    // width changes as well as plain copies.
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1) {
      V = Op.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
      V = Op.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2) {
      // The mask may be either operand (AND is commutative) and may be
      // materialised in a register — ARM `and rd, rMask, rVal,lsl#s` cannot
      // encode an immediate alongside the shifted value — so resolve each
      // operand to a constant directly or through a COPY chain.
      auto constOf = [&](NdVar X) -> std::optional<uint64_t> {
        for (int G = 0, F = D - 1; G < limits::kMaxQuasiCopyDepth; ++G) {
          if (X.isConst())
            return X.Offset;
          if (!X.isReg() && !X.isTemp())
            return std::nullopt;
          int DD = reachingDefIdx(Ops, F, X);
          if (DD < 0 || Ops[DD].Opcode != NdOp::COPY || Ops[DD].NumInputs < 1)
            return std::nullopt;
          X = Ops[DD].Inputs[0];
          F = DD - 1;
        }
        return std::nullopt;
      };
      for (int W = 0; W < 2; ++W) {
        auto MOpt = constOf(Op.Inputs[W]);
        if (!MOpt || *MOpt == 0)
          continue;
        uint64_t M = *MOpt;
        uint32_t K = 0;
        for (uint64_t T = M; (T & 1) == 0; T >>= 1)
          ++K;
        uint64_t Low = M >> K; // contiguous bit run if (Low & (Low+1)) == 0
        if ((Low & (Low + 1)) == 0 && (1ull << K) == EntrySize)
          return true;
      }
      return false;
    }
    return false;
  }
  return false;
}

bool CFGBuilder::detectUnscaledRelocTableLoad(
    const BinaryImage &Img, const InsnRecord &Rec, uint64_t &BaseReg,
    uint64_t &IndexReg, uint16_t &LoadWidth, uint64_t &Disp,
    va_t &TableAddr) const {
  // Flatten the whole function prefix up to the dispatch so the pre-scaling
  // mask (often hoisted into a register in the loop preheader) is in scope.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
    const LowOp &L = Ops[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    // A computed-goto label table holds pointer-width absolute targets.
    uint16_t W = L.Output.Size;
    if (W != 4 && W != 8)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    // Resolve the load address (through COPY) to its defining INT_ADD.
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

    // Two address shapes carry a pre-scaled index:
    //   * plain (x86-64): INT_ADD(table_base_reg, index)
    //   * GOTOFF (i386):  INT_ADD(INT_ADD(got_base_reg, index), disp); the
    //     loader baked the table VA into `disp`, so the table sits there.
    int InnerIdx = AddIdx;
    uint64_t LocalDisp = 0;
    for (int Which = 0; Which < 2; ++Which) {
      if (!Ops[AddIdx].Inputs[Which].isConst())
        continue;
      LocalDisp = Ops[AddIdx].Inputs[Which].Offset;
      int Inner =
          reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
      for (int G = 0;
           Inner >= 0 && Ops[Inner].Opcode == NdOp::COPY &&
           Ops[Inner].NumInputs >= 1 &&
           (Ops[Inner].Inputs[0].isReg() || Ops[Inner].Inputs[0].isTemp()) &&
           G < limits::kMaxQuasiCopyDepth;
           ++G)
        Inner = reachingDefIdx(Ops, Inner - 1, Ops[Inner].Inputs[0]);
      InnerIdx = Inner;
      break;
    }
    if (InnerIdx < 0 || Ops[InnerIdx].Opcode != NdOp::INT_ADD ||
        Ops[InnerIdx].NumInputs < 2 || Ops[InnerIdx].Inputs[0].isConst() ||
        Ops[InnerIdx].Inputs[1].isConst())
      continue;

    va_t LoadAddr = L.Addr;
    const LowOp &Sum = Ops[InnerIdx];
    for (int Side = 0; Side < 2; ++Side) {
      // The pre-scaling mask, whose stride equals the entry size, identifies
      // the index unambiguously; the other operand is the table / GOT base.
      if (!isPreScaledIndex(Ops, InnerIdx - 1, Sum.Inputs[Side], W))
        continue;
      uint64_t Idx = traceToRegister(Ops, InnerIdx - 1, Sum.Inputs[Side]);
      uint64_t Base = traceToRegister(Ops, InnerIdx - 1, Sum.Inputs[1 - Side]);
      if (Idx == InvalidVA || Base == InvalidVA)
        continue;
      // Confirm a code-pointer relocation run at the table VA: GOTOFF tables
      // sit at `disp`; plain tables at the folded base register.  This is the
      // verifiable label-table signature no ordinary load shares.
      va_t Table = LocalDisp;
      if (LocalDisp == 0) {
        auto BaseAddrOpt = foldRegConstant(Img, Rec, Base, LoadAddr);
        if (!BaseAddrOpt || *BaseAddrOpt == 0)
          continue;
        Table = *BaseAddrOpt;
      }
      if (countCodePtrRelocRun(Img, Table, W) < limits::kMinJumpTableEntries)
        continue;
      BaseReg = Base;
      IndexReg = Idx;
      LoadWidth = W;
      Disp = LocalDisp;
      TableAddr = Table;
      return true;
    }
  }
  return false;
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
      // folds.
      if (Cur.isReg()) {
        auto F = foldRegConstant(Img, Rec, Cur.Offset, Cutoff);
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
        uint16_t Bytes = O.Output.Size;
        if (Bytes == 0 || Bytes >= sizeof(uint64_t))
          return F;
        uint16_t Bits = static_cast<uint16_t>(Bytes * 8);
        return *F & ((uint64_t{1} << Bits) - 1);
      }
      if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
        auto A = foldArm(O.Inputs[0], D - 1, Cutoff, Depth + 1);
        auto B = foldArm(O.Inputs[1], D - 1, Cutoff, Depth + 1);
        if (A && B)
          return *A + *B;
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

bool CFGBuilder::tryCrossInstrRelativeTable(const BinaryImage &Img,
                                            const InsnRecord &Rec,
                                            JumpTableInfo &Info) {
  // A memory-indirect jump (`jmp *(base,idx,8)`, the computed-goto / threaded
  // dispatch shape) lifts to a LOAD feeding an INDIR_BR whose input is a
  // *temp*; a register-indirect jump (`jmp *rax`) feeds a register.  The table
  // analysis below works off the LOAD either way, so only require that an
  // indirect branch with an input is present.
  bool HasIndBranch = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  if (!HasIndBranch)
    return false;

  // Flatten the INDIR_BR block ops; the table base may be a block live-in.
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

  uint64_t BaseReg = InvalidVA;
  uint64_t IndexReg = InvalidVA;
  uint16_t LoadWidth = 0;
  bool HasScaledIndex = false;
  bool SawSext = false;
  bool Unscaled = false;
  bool CrossBlockReloc = false;
  int LoadIdx = -1;
  uint64_t Disp = 0;
  va_t CrossBlockAddVA = InvalidVA;

  for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I) {
    auto &L = BlockOps[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    uint16_t W = L.Output.Size;
    if (W != 1 && W != 2 && W != 4 && W != 8)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    if (analyzeTableLoadAddr(BlockOps, I - 1, AddrV, BaseReg, IndexReg,
                             HasScaledIndex, Disp)) {
      LoadWidth = W;
      LoadIdx = I;
      for (int K = I + 1; K < static_cast<int>(BlockOps.size()); ++K)
        if (BlockOps[K].Opcode == NdOp::INT_SEXT)
          SawSext = true;
      break;
    }
  }

  // -O0 shared/decoupled dispatch (`&&label` computed goto): the indirect
  // branch reloads a spilled target (`ldr xT,[sp,#k]; br xT`) whose scaled
  // table load sits in a *predecessor* goto-site block
  // (`... ldr xT,[base,idx,scale]; str xT,[sp,#k]; b dispatch`).  The own-block
  // scan above sees only the frame reload, so widen the op window to the
  // single-predecessor path and rescan for the predecessor's scaled table load.
  // Acceptance is gated on an absolute code-pointer reloc run below, so a
  // register-indirect function-pointer tail call (no reloc table) is never
  // misread as a jump table.
  if (BaseReg == InvalidVA) {
    std::vector<LowOp> PathOps = collectPathOps(BlkStart, Rec.Addr);
    if (PathOps.size() > BlockOps.size()) {
      for (int I = static_cast<int>(PathOps.size()) - 1; I >= 0; --I) {
        auto &L = PathOps[I];
        if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
          continue;
        uint16_t W = L.Output.Size;
        if (W != 1 && W != 2 && W != 4 && W != 8)
          continue;
        const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
        if (!AddrV.isReg() && !AddrV.isTemp())
          continue;
        if (analyzeTableLoadAddr(PathOps, I - 1, AddrV, BaseReg, IndexReg,
                                 HasScaledIndex, Disp, &CrossBlockAddVA)) {
          LoadWidth = W;
          LoadIdx = I;
          for (int K = I + 1; K < static_cast<int>(PathOps.size()); ++K)
            if (PathOps[K].Opcode == NdOp::INT_SEXT)
              SawSext = true;
          BlockOps = std::move(PathOps);
          CrossBlockReloc = true;
          break;
        }
      }
    }
  }

  // No scaled index found: try the pre-scaled (scale-1) computed-goto form,
  // where the size optimizer folded the entry-size scale into the index itself.
  va_t UnscaledTableAddr = 0;
  if (BaseReg == InvalidVA)
    Unscaled = detectUnscaledRelocTableLoad(Img, Rec, BaseReg, IndexReg,
                                            LoadWidth, Disp, UnscaledTableAddr);

  if (BaseReg == InvalidVA || LoadWidth == 0 || (!HasScaledIndex && !Unscaled))
    return false;

  // The index register at the table load (e.g. RCX in `movslq (rdx,rcx,4)`) is
  // frequently a fresh copy of the original switch variable (`mov ecx,edi`).
  // Trace it back to that source register so the guard `cmp edi,N` is matched
  // and unrelated flag masks (e.g. the parity `and x,1`) are not mistaken for
  // the table bound.  The pre-scaled form already resolved its terminal index
  // register, so skip these block-local refinements for it.
  if (!Unscaled && IndexReg != InvalidVA && LoadIdx > 0)
    IndexReg = traceRegSource(BlockOps, LoadIdx - 1, IndexReg);

  // At -O0 the guarded switch variable is spilled before the dispatch block and
  // reloaded into the index register (`str rX,[sp,#k]; ... ldr rIdx,[sp,#k]`),
  // so the `cmp`/`and` bound sits on `rX`, not the reloaded index.  Forward the
  // index through the stack slot to `rX` so the bound analysis connects — the
  // .text-embedded ARM PC-relative table carries no relocation run to bound it,
  // so without this it stays unbounded and the dispatch degrades to a tail
  // call.
  if (!Unscaled && IndexReg != InvalidVA && LoadIdx > 0)
    IndexReg =
        forwardIndexThroughStackSpill(BlockOps, LoadIdx, IndexReg, BlkStart);

  // The register added back to each loaded entry is the relative base for the
  // targets.  Three shapes occur:
  //
  //   * Plain PIC table (x64 `lea table(%rip),%rN; movslq (%rN,%idx,4),%r8;
  //     add %rN,%r8`): the register *is* the table, so Disp == 0 and the target
  //     is table_base + entry.  Fold the register to the table address.
  //
  //   * i386 PIC GOTOFF table (`mov disp(%ecx,%idx,4),%r; add %ecx,%r`): the
  //     register is the GOT base, which the loader treats as the image base 0,
  //     and the loader already baked that base into the resolved GOTOFF Disp.
  //     The GOT base is routinely spilled to a stack slot and reloaded, so a
  //     prefix-emulation fold of it is unreliable; use the image-base GOT (0)
  //     so the table sits at Disp and the target is 0 + entry.
  //
  //   * Pre-scaled computed goto: detectUnscaledRelocTableLoad already resolved
  //     the table VA (folded base, or GOTOFF disp) and confirmed its reloc run.
  uint64_t TableAddr;
  // A frame-register base (SP/FP) with a load displacement that is a *frame
  // offset* (not a data-segment VA) marks a stack-materialised local table: the
  // x86-64/i386 -O0 frame offset rides in the load displacement (`mov
  // (%rbp,%idx,8),-0x30`), so route it to the stack resolver (threading Disp in
  // as the frame offset).  But i386 -O2 PIC repurposes EBP as the GOT base with
  // a GOTOFF displacement that *is* a real data-segment table VA (`mov
  // 0x9c(%ebp,%idx,4),%r`) — that is a regular relative table, not a stack one
  // — so only divert a frame base when the displacement does not resolve to a
  // data segment.  (A genuine non-frame GOT-base table keeps GotOff
  // regardless.)
  const TargetRegInfo &TRIcg = getTargetRegInfo(Img.Arch);
  bool BaseIsFrame = BaseReg != InvalidVA && TRIcg.isFrameReg(BaseReg);
  bool DispIsDataVA = Disp != 0 && Img.getSegmentFor(Disp) != nullptr;
  bool GotOff = Disp != 0 && (!BaseIsFrame || DispIsDataVA);
  if (Unscaled) {
    TableAddr = UnscaledTableAddr;
  } else if (GotOff) {
    TableAddr = Disp;
  } else {
    // Fold the base register at the table load, not at the branch: the base may
    // be reused after the target is computed (`add %r11,%r10; mov %edx,%r11d;
    // jmp *%r10`), and emulating past that clobber would read the wrong value.
    // For a cross-block decoupled dispatch the base register is also clobbered
    // *before* the load (ARM `add rB,rB,idx,lsl#k; ldr [rB]`), so fold it at
    // the base+index add — before the index is folded in — to read the pure
    // base.
    va_t LoadAddr = (LoadIdx >= 0) ? BlockOps[LoadIdx].Addr : InvalidVA;
    va_t FoldAt = (CrossBlockReloc && CrossBlockAddVA != InvalidVA)
                      ? CrossBlockAddVA
                      : LoadAddr;
    // A local (non-`static`) computed-goto table is materialised on the stack
    // at -O0: the base register is a frame slot (SP/FP + const) into which the
    // read-only initializer run was copied.  Such a base folds to a bogus
    // low/stack address rather than a data segment, so try the stack-table
    // resolver first — it is gated on a frame-register base *and* a
    // code-pointer reloc run at the traced init source, returning InvalidVA for
    // any non-stack base, so a normal PIC/lea base falls through to the fold
    // below.
    bool StackTableMutated = false;
    va_t StackSrc = resolveStackMaterializedTableSource(
        Img, Rec, BlockOps, LoadIdx, BaseReg, LoadWidth,
        static_cast<int64_t>(Disp), &StackTableMutated);
    if (StackSrc != InvalidVA) {
      TableAddr = StackSrc;
      // A runtime-permuted stack table keeps its (static) targets so the
      // dispatch retains successors, but is marked so the emitter traps rather
      // than dispatching on the now-stale index->target map.
      Info.MutatedUnsafe = StackTableMutated;
    } else {
      std::optional<uint64_t> RelBaseOpt =
          foldRegConstant(Img, Rec, BaseReg, FoldAt);
      if (!RelBaseOpt || *RelBaseOpt == 0)
        return false;
      TableAddr = *RelBaseOpt;
    }
  }

  const auto *Seg = Img.getSegmentFor(TableAddr);
  if (!Seg || Seg->Data.empty())
    return false;

  Info.BaseAddr = TableAddr;
  Info.EntrySize = LoadWidth;
  Info.IndexReg = IndexReg;

  // Reloc-driven absolute table: a run of loader-applied absolute code-pointer
  // relocations starting at the base means the entries are absolute targets and
  // the run length is the exact entry count.  This recovers a computed-goto /
  // threaded dispatch (which has no comparison guard to bound it) and resolves
  // the 4-byte absolute-vs-PIC-relative ambiguity — a PIC switch table carries
  // no relocations on its entries.
  uint32_t RelocRun = countCodePtrRelocRun(Img, TableAddr, LoadWidth);
  // The pre-scaled form is only sound when a relocation run confirms the table;
  // without one there is no valid decoding for a bare base+index load.
  if (Unscaled && RelocRun < limits::kMinJumpTableEntries)
    return false;
  // A cross-block (decoupled-dispatch) table is trusted only when an absolute
  // code-pointer reloc run confirms it; otherwise the widened op window could
  // latch a coincidental scaled load on a non-jump-table indirect branch.
  if (CrossBlockReloc && RelocRun < limits::kMinJumpTableEntries)
    return false;
  if (RelocRun >= limits::kMinJumpTableEntries) {
    Info.IsRelative = false;
    Info.IsSigned = false;
    Info.MaxEntries = RelocRun;
    Info.RelocAbsolute = true;
    Info.RelocBounded = true;
    // A pre-scaled index already byte-scales the entry (`table + entry*size`),
    // so its case values are the byte offsets {0, size, 2*size, ...} rather
    // than 0..N-1; record the stride so recoverCaseLabels emits matching
    // labels.
    if (Unscaled) {
      Info.Stride = LoadWidth;
      Info.PreScaledIndex = true;
    }
  } else if (GotOff) {
    // GOTOFF entries are target - GOT_base; with GOT_base = 0 they equal the
    // absolute target addresses.
    Info.IsRelative = false;
    Info.IsSigned = false;
  } else {
    Info.IsRelative = (LoadWidth < limits::kMaxEntryBytes);
    Info.IsSigned = SawSext || (LoadWidth < limits::kMaxEntryBytes);
  }

  // AArch64 -O0 word tables add the signed entry to a separate `adr` anchor
  // rather than to the table base (`adr xA,.L; ldrsw xO,[tbl,idx,4]; add xA,xA,
  // xO; br xA`), so the target is anchor+entry, not table+entry.  Detect the
  // add feeding the branch whose other operand is a pure code-address constant
  // (an `adr`, traced through COPY only — never a LOAD, which is the
  // entry/offset) distinct from the (rodata) table, and use it as the target
  // base.  x64 PIC (added operand is the non-executable rodata table) and ARM32
  // (anchor *is* the table base) do not match, so they keep table-relative
  // targets.
  if (Info.IsRelative && Info.TargetBase == 0) {
    uint64_t BrReg = InvalidVA;
    for (auto &Op : Rec.Ops)
      if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
          Op.Inputs[0].isReg()) {
        BrReg = Op.Inputs[0].Offset;
        break;
      }
    int AddD = -1;
    if (BrReg != InvalidVA)
      for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I)
        if (BlockOps[I].Opcode == NdOp::INT_ADD && BlockOps[I].Output.isReg() &&
            BlockOps[I].Output.Offset == BrReg && BlockOps[I].NumInputs >= 2) {
          AddD = I;
          break;
        }
    if (AddD >= 0) {
      va_t AddAddr = BlockOps[AddD].Addr;
      auto traceConstAnchor = [&](NdVar V,
                                  int From) -> std::optional<uint64_t> {
        for (int G = 0; G < limits::kMaxSliceDepth; ++G) {
          if (V.isConst())
            return V.Offset;
          if (!V.isReg() && !V.isTemp())
            return std::nullopt;
          int D = reachingDefIdx(BlockOps, From, V);
          if (D < 0) {
            if (V.isReg()) {
              auto F = foldRegConstant(Img, Rec, V.Offset, AddAddr);
              if (F && *F)
                return *F;
            }
            return std::nullopt;
          }
          const LowOp &O = BlockOps[D];
          if (O.Opcode != NdOp::COPY || O.NumInputs < 1)
            return std::nullopt;
          if (O.Inputs[0].isConst())
            return O.Inputs[0].Offset;
          V = O.Inputs[0];
          From = D - 1;
        }
        return std::nullopt;
      };
      for (int W = 0; W < 2; ++W) {
        auto Anchor = traceConstAnchor(BlockOps[AddD].Inputs[W], AddD - 1);
        if (!Anchor || *Anchor == 0 || *Anchor == TableAddr)
          continue;
        const auto *AS = Img.getSegmentFor(*Anchor);
        if (AS && AS->isExecutable()) {
          Info.TargetBase = *Anchor;
          Info.EntryScale = 1;
          break;
        }
      }
    }
  }
  return true;
}

} // namespace neverd
