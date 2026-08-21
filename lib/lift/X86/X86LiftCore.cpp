//===- X86LiftCore.cpp - x86/x64 core instruction lifter ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the core x86/x64 integer ALU instructions to the per-family
/// handlers in X86LiftCore*.cpp.  DIV/IDIV and the bit-test family stay here:
/// they read the lifter's private cross-instruction state (the CQO/CDQ + DIV
/// tracking and the target architecture).  Also defines the carry snapshot
/// helper that ADC/SBB and RCL/RCR share.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

NdVar snapshotCarryAtWidth(X86Lifter::LiftState &S, uint16_t Size) {
  NdVar Carry = NdVar::reg(x86reg::CF, 1);
  NdVar Snapshot = S.makeTemp(Size);
  // ADC/SBB still consume the incoming carry while computing OF after they
  // have written the new CF. Keep a stable temp even for byte operations;
  // only the width conversion disappears in that case.
  S.emit(Size == 1 ? NdOp::COPY : NdOp::INT_ZEXT, Snapshot, {Carry});
  return Snapshot;
}

bool X86Lifter::liftCore(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_DIV: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t Sz = Src.Size;
    if (Sz == 1) {
      NdVar Ax = NdVar::reg(x86reg::RAX, 2);
      NdVar ExtSrc = S.makeTemp(2);
      S.emit(NdOp::INT_ZEXT, ExtSrc, {Src});
      NdVar Quot = S.makeTemp(2);
      NdVar Rem = S.makeTemp(2);
      S.emit(NdOp::INT_DIV, Quot, {Ax, ExtSrc});
      S.emit(NdOp::INT_REM, Rem, {Ax, ExtSrc});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX, 1),
             {Quot, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX + 1, 1),
             {Rem, NdVar::scalar(0, 4)});
    } else {
      NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
      NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);

      bool IsZeroRdx = (LastRdxState == RdxState::Zero && LastRdxSize >= Sz);

      if (IsZeroRdx) {
        NdVar OrigRax = S.makeTemp(Sz);
        S.emit(NdOp::COPY, OrigRax, {Rax});
        S.emit(NdOp::INT_DIV, Rax, {OrigRax, Src});
        S.emit(NdOp::INT_REM, Rdx, {OrigRax, Src});
      } else {
        // The double-width INT_DIV/INT_REM below is recognized by the value
        // emitter and lowered back to a single `divq` via inline-asm
        // passthrough (no __udivti3/__umodti3 libcall) — original binary stays
        // binary.
        LLVM_DEBUG(if (Sz * 2 > 8) llvm::dbgs()
                   << "DIV wide fallback: i" << (Sz * 2 * 8)
                   << " udiv/urem at 0x" << llvm::utohexstr(S.Addr)
                   << " — emitter lowers to inline-asm div\n");
        NdVar ExtRAX = S.makeTemp(Sz * 2);
        NdVar ExtRDX = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_ZEXT, ExtRAX, {Rax});
        S.emit(NdOp::INT_ZEXT, ExtRDX, {Rdx});
        NdVar HiShifted = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_LEFT, HiShifted,
               {ExtRDX, NdVar::scalar(Sz * 8, Sz * 2)});
        NdVar Dividend = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_OR, Dividend, {HiShifted, ExtRAX});
        NdVar ExtSrc = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_ZEXT, ExtSrc, {Src});
        NdVar Quot = S.makeTemp(Sz * 2);
        NdVar Rem = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_DIV, Quot, {Dividend, ExtSrc});
        S.emit(NdOp::INT_REM, Rem, {Dividend, ExtSrc});
        S.emit(NdOp::SUBBYTES, Rax, {Quot, NdVar::scalar(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Rem, NdVar::scalar(0, 4)});
      }
    }
    break;
  }
  case X86_INS_IDIV: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t Sz = Src.Size;
    if (Sz == 1) {
      NdVar Ax = NdVar::reg(x86reg::RAX, 2);
      NdVar ExtSrc = S.makeTemp(2);
      S.emit(NdOp::INT_SEXT, ExtSrc, {Src});
      NdVar Quot = S.makeTemp(2);
      NdVar Rem = S.makeTemp(2);
      S.emit(NdOp::INT_SDIV, Quot, {Ax, ExtSrc});
      S.emit(NdOp::INT_SREM, Rem, {Ax, ExtSrc});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX, 1),
             {Quot, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX + 1, 1),
             {Rem, NdVar::scalar(0, 4)});
    } else {
      NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
      NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);

      bool IsCqoIdiom =
          (LastRdxState == RdxState::SignExtRAX && LastRdxSize == Sz);

      if (IsCqoIdiom) {
        NdVar OrigRax = S.makeTemp(Sz);
        S.emit(NdOp::COPY, OrigRax, {Rax});
        S.emit(NdOp::INT_SDIV, Rax, {OrigRax, Src});
        S.emit(NdOp::INT_SREM, Rdx, {OrigRax, Src});
      } else {
        // The double-width INT_SDIV/INT_SREM below is recognized by the value
        // emitter and lowered back to a single `idiv` via inline-asm
        // passthrough (no __divti3/__modti3 libcall) — original binary stays
        // binary.
        LLVM_DEBUG(if (Sz * 2 > 8) llvm::dbgs()
                   << "IDIV wide fallback: i" << (Sz * 2 * 8)
                   << " sdiv/srem at 0x" << llvm::utohexstr(S.Addr)
                   << " — emitter lowers to inline-asm idiv\n");
        NdVar ExtRAX = S.makeTemp(Sz * 2);
        NdVar ExtRDX = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_ZEXT, ExtRAX, {Rax});
        S.emit(NdOp::INT_SEXT, ExtRDX, {Rdx});
        NdVar HiShifted = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_LEFT, HiShifted,
               {ExtRDX, NdVar::scalar(Sz * 8, Sz * 2)});
        NdVar Dividend = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_OR, Dividend, {HiShifted, ExtRAX});
        NdVar ExtSrc = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SEXT, ExtSrc, {Src});
        NdVar Quot = S.makeTemp(Sz * 2);
        NdVar Rem = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SDIV, Quot, {Dividend, ExtSrc});
        S.emit(NdOp::INT_SREM, Rem, {Dividend, ExtSrc});
        S.emit(NdOp::SUBBYTES, Rax, {Quot, NdVar::scalar(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Rem, NdVar::scalar(0, 4)});
      }
    }
    break;
  }

  // --- Bit test/set/reset/complement ---
  // The bit offset's meaning depends on the bit-base operand and offset kind:
  //   * register base, or any immediate offset: the offset is taken modulo the
  //     operand width (16/32/64), so `bt eax,33` tests bit 1.
  //   * register offset on a MEMORY base: the offset is a signed bit index into
  //     a bit string — the accessed operand-size chunk is at
  //     EA + ((sext(idx) >> log2(bits)) << log2(bytes)), and the in-chunk bit
  //     is idx & (bits-1).  (Matches the QEMU/x86 reference for `bt mem,reg`.)
  case X86_INS_BT:
  case X86_INS_BTS:
  case X86_INS_BTR:
  case X86_INS_BTC: {
    if (X86.op_count < 2)
      break;
    uint16_t Sz = static_cast<uint16_t>(X86.operands[0].size);
    if (Sz == 0)
      Sz = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t Bits = Sz * 8;
    uint64_t LogSz = (Sz == 8) ? 3 : (Sz == 4) ? 2 : (Sz == 2) ? 1 : 0;
    uint16_t PtrSz = (TargetArch == Arch::X64) ? 8 : 4;
    bool MemBase = (X86.operands[0].type == X86_OP_MEM);
    bool RegOffset = (X86.operands[1].type == X86_OP_REG);

    NdVar IdxRaw = operandRead(S, X86.operands[1]);

    // Locate the operand-size value containing the target bit, and (for memory)
    // the byte address to load/store.
    NdVar Base;
    NdVar ByteAddr;
    if (MemBase) {
      ByteAddr = S.computeEA(X86.operands[0]);
      if (RegOffset) {
        NdVar IdxExt = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_SEXT, IdxExt, {IdxRaw});
        NdVar ChunkOff = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_ASHR, ChunkOff,
               {IdxExt, NdVar::scalar(LogSz + 3, PtrSz)});
        NdVar ByteOff = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_LEFT, ByteOff,
               {ChunkOff, NdVar::scalar(LogSz, PtrSz)});
        NdVar Adj = S.makeTemp(PtrSz);
        S.emit(NdOp::INT_ADD, Adj, {ByteAddr, ByteOff});
        ByteAddr = Adj;
      }
      Base = S.makeTemp(Sz);
      S.emit(NdOp::LOAD, Base, {ByteAddr});
    } else {
      Base = operandRead(S, X86.operands[0]);
    }

    // In-chunk bit position, taken modulo the operand width.
    NdVar Idx = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Idx, {IdxRaw, NdVar::scalar(Bits - 1, Sz)});

    // CF = (Base >> Idx) & 1
    NdVar Shifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shifted, {Base, Idx});
    NdVar Masked = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Masked, {Shifted, NdVar::scalar(1, Sz)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {Masked, NdVar::scalar(0, Sz)});

    if (InsnId != X86_INS_BT) {
      NdVar Mask = S.makeTemp(Sz);
      S.emit(NdOp::INT_LEFT, Mask, {NdVar::scalar(1, Sz), Idx});
      NdVar Result = (MemBase || X86.operands[0].type == X86_OP_MEM)
                         ? S.makeTemp(Sz)
                         : operandWrite(X86.operands[0]);
      if (InsnId == X86_INS_BTS)
        S.emit(NdOp::INT_OR, Result, {Base, Mask});
      else if (InsnId == X86_INS_BTR) {
        NdVar Inv = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, Inv, {Mask});
        S.emit(NdOp::INT_AND, Result, {Base, Inv});
      } else { // BTC
        S.emit(NdOp::INT_XOR, Result, {Base, Mask});
      }
      if (MemBase)
        S.emit(NdOp::STORE, {}, {ByteAddr, Result});
    }
    break;
  }

  default:
    return liftCoreMove(*this, S, Insn, X86) ||
           liftCoreArith(*this, S, Insn, X86) ||
           liftCoreShift(*this, S, Insn, X86) ||
           liftCoreBit(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
