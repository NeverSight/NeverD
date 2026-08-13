//===- X86LiftCoreBit.cpp - x86/x64 bit scan and flag lifter --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bit scan forward/reverse, BSWAP, and the EFLAGS
/// instructions: CLC/STC/CMC, CLD/STD, SAHF/LAHF and the
/// CWD/CWDE/CBW accumulator widenings.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftCoreBit(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                 const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- BSF / BSR ---
  // When the source is zero, ZF=1 and the destination is left UNCHANGED.  The
  // Intel manual labels the destination "undefined" in that case, but real
  // hardware (and QEMU/Unicorn, via a conditional move) preserve the prior
  // destination value, and real programs depend on it.  The old code wrote the
  // computed value unconditionally — which for a zero source is (Bits-1)-clz(0)
  // = -1, clobbering the destination.  Select the old destination on src==0.
  case X86_INS_BSF:
  case X86_INS_BSR: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar OldDst = L.operandRead(S, X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar SrcZero = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, SrcZero, {Src, NdVar::cst(0, Src.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {SrcZero});
    NdVar Computed = S.makeTemp(DstW.Size);
    if (InsnId == X86_INS_BSR) {
      // BSR: index of highest set bit = (Bits-1) - CLZ(Src)
      NdVar Clz = S.makeTemp(DstW.Size);
      S.emit(NdOp::LZCOUNT, Clz, {Src});
      S.emit(NdOp::INT_SUB, Computed,
             {NdVar::cst(DstW.Size * 8 - 1, DstW.Size), Clz});
    } else {
      // BSF: index of lowest set bit = (Bits-1) - CLZ(Src & -Src)
      // isolate lowest bit: low = Src & (-Src)
      NdVar Neg = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      NdVar LowBit = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_AND, LowBit, {Src, Neg});
      NdVar Clz = S.makeTemp(DstW.Size);
      S.emit(NdOp::LZCOUNT, Clz, {LowBit});
      S.emit(NdOp::INT_SUB, Computed,
             {NdVar::cst(DstW.Size * 8 - 1, DstW.Size), Clz});
    }
    // Preserve the destination when the source is zero (matches hardware/QEMU).
    S.emit(NdOp::SELECT, DstW, {SrcZero, OldDst, Computed});
    break;
  }

  // --- BSWAP (byte swap): emit as a chain of shifts+ORs. For 32/64-bit
  //     register operand, swap all bytes. Implementation here is a
  //     pragmatic shift-sequence; downstream NdOp → LLVM IR doesn't have
  //     a native bswap opcode anyway, so a sequence is fine.
  case X86_INS_BSWAP: {
    if (X86.op_count < 1)
      break;
    NdVar Src = L.operandRead(S, X86.operands[0]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Swapped = S.emitByteSwap(Src);
    S.emit(NdOp::COPY, Dst, {Swapped});
    break;
  }

  // ========================================================================
  // Flag manipulation
  // ========================================================================
  case X86_INS_CLC:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
    break;
  case X86_INS_STC:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(1, 1)});
    break;
  case X86_INS_CMC: {
    NdVar NC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NC});
    break;
  }
  case X86_INS_CLD:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::DF, 1), {NdVar::cst(0, 1)});
    break;
  case X86_INS_STD:
    S.emit(NdOp::COPY, NdVar::reg(x86reg::DF, 1), {NdVar::cst(1, 1)});
    break;
  case X86_INS_SAHF: {
    NdVar Ah = NdVar::reg(x86reg::RAX + 1, 1);
    NdVar BitCf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitCf, {Ah, NdVar::cst(0x01, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {BitCf, NdVar::cst(0, 1)});
    NdVar BitPf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitPf, {Ah, NdVar::cst(0x04, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::PF, 1),
           {BitPf, NdVar::cst(0, 1)});
    NdVar BitZf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitZf, {Ah, NdVar::cst(0x40, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::ZF, 1),
           {BitZf, NdVar::cst(0, 1)});
    NdVar BitSf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitSf, {Ah, NdVar::cst(0x80, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::SF, 1),
           {BitSf, NdVar::cst(0, 1)});
    NdVar BitAf = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitAf, {Ah, NdVar::cst(0x10, 1)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::AF, 1),
           {BitAf, NdVar::cst(0, 1)});
    break;
  }
  case X86_INS_LAHF: {
    NdVar Ah = NdVar::reg(x86reg::RAX + 1, 1);
    NdVar SfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, SfSh, {NdVar::reg(x86reg::SF, 1), NdVar::cst(7, 1)});
    NdVar ZfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, ZfSh, {NdVar::reg(x86reg::ZF, 1), NdVar::cst(6, 1)});
    NdVar PfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, PfSh, {NdVar::reg(x86reg::PF, 1), NdVar::cst(2, 1)});
    NdVar AfSh = S.makeTemp(1);
    S.emit(NdOp::INT_LEFT, AfSh, {NdVar::reg(x86reg::AF, 1), NdVar::cst(4, 1)});
    NdVar M1 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M1, {SfSh, ZfSh});
    NdVar M2 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M2, {M1, PfSh});
    NdVar M2a = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M2a, {M2, AfSh});
    NdVar M3 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, M3, {M2a, NdVar::cst(0x02, 1)});
    S.emit(NdOp::INT_OR, Ah, {M3, NdVar::reg(x86reg::CF, 1)});
    break;
  }

  // ========================================================================
  // Sign-extension siblings of CDQ/CQO/CDQE
  // ========================================================================
  case X86_INS_CWD:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 2),
           {NdVar::reg(x86reg::RAX, 2), NdVar::cst(15, 2)});
    break;
  case X86_INS_CWDE:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 4),
           {NdVar::reg(x86reg::RAX, 2)});
    break;
  case X86_INS_CBW:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 2),
           {NdVar::reg(x86reg::RAX, 1)});
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
