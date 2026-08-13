//===- JumpTableResolverStack.cpp - Stack table materialization ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack-spill forwarding and recovery of jump tables materialized into local
/// frame storage from a constant initializer run.
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
#include "neverd/libc/LibCNames.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// forwardIndexThroughStackSpill — reconnect a guarded spilled index
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// resolveStackMaterializedTableSource — recover a local table initializer
//===----------------------------------------------------------------------===//

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

} // namespace neverd
