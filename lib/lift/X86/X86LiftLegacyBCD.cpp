//===- X86LiftLegacyBCD.cpp - x86 BCD and 8086-legacy instruction lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The 32-bit-only legacy integer instructions: the BCD
/// adjust family (AAA/AAS/AAD/AAM/DAA/DAS), the BOUND range
/// check, INTO, the undocumented SALC, and the far-pointer
/// loads (LDS/LES/LSS/LFS/LGS).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftLegacyBCD(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // BCD legacy (AAA/AAS/AAD/AAM/DAA/DAS, 32-bit only).  Modelled in MedIR to
  // match QEMU/Unicorn bit-for-bit; the prior placeholders emitted an intrinsic
  // then clobbered AX with an uninitialised temp (garbage result, no flags).
  case X86_INS_AAA:
  case X86_INS_AAS: {
    bool IsSub = (InsnId == X86_INS_AAS);
    NdVar AL = NdVar::reg(x86reg::RAX, 1);
    NdVar AH = NdVar::reg(x86reg::RAX + 1, 1);
    NdVar OldAL = S.makeTemp(1), OldAH = S.makeTemp(1), Af = S.makeTemp(1);
    S.emit(NdOp::COPY, OldAL, {AL});
    S.emit(NdOp::COPY, OldAH, {AH});
    S.emit(NdOp::COPY, Af, {NdVar::reg(x86reg::AF, 1)});
    // cond = ((AL & 0xF) > 9) || AF
    NdVar Lo = S.makeTemp(1);
    S.emit(NdOp::INT_AND, Lo, {OldAL, NdVar::cst(0x0F, 1)});
    NdVar LoGt9 = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, LoGt9, {NdVar::cst(9, 1), Lo});
    NdVar Cond = S.makeTemp(1);
    S.emit(NdOp::INT_OR, Cond, {LoGt9, Af});
    // AH carry-in: AAA when AL+6 overflows the byte (AL>0xF9); AAS when AL<6.
    NdVar ICarry = S.makeTemp(1);
    if (IsSub)
      S.emit(NdOp::INT_LESS, ICarry, {OldAL, NdVar::cst(6, 1)});
    else
      S.emit(NdOp::INT_LESS, ICarry, {NdVar::cst(0xF9, 1), OldAL});
    // AL' = (cond ? AL±6 : AL) & 0xF
    NdVar AlAdj = S.makeTemp(1);
    S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, AlAdj,
           {OldAL, NdVar::cst(6, 1)});
    NdVar AlSel = S.makeTemp(1);
    S.emit(NdOp::SELECT, AlSel, {Cond, AlAdj, OldAL});
    NdVar AlNew = S.makeTemp(1);
    S.emit(NdOp::INT_AND, AlNew, {AlSel, NdVar::cst(0x0F, 1)});
    // AH' = cond ? AH ±(1 + icarry) : AH
    NdVar Step = S.makeTemp(1);
    S.emit(NdOp::INT_ADD, Step, {NdVar::cst(1, 1), ICarry});
    NdVar AhAdj = S.makeTemp(1);
    S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, AhAdj, {OldAH, Step});
    NdVar AhSel = S.makeTemp(1);
    S.emit(NdOp::SELECT, AhSel, {Cond, AhAdj, OldAH});
    S.emit(NdOp::COPY, AL, {AlNew});
    S.emit(NdOp::COPY, AH, {AhSel});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {Cond});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::AF, 1), {Cond});
    break;
  }
  case X86_INS_DAA:
  case X86_INS_DAS: {
    bool IsSub = (InsnId == X86_INS_DAS);
    NdVar AL = NdVar::reg(x86reg::RAX, 1);
    NdVar OldAL = S.makeTemp(1), Cf = S.makeTemp(1), Af = S.makeTemp(1);
    S.emit(NdOp::COPY, OldAL, {AL});
    S.emit(NdOp::COPY, Cf, {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::COPY, Af, {NdVar::reg(x86reg::AF, 1)});
    // cond1 = ((AL & 0xF) > 9) || AF  -> nibble adjust by ±6
    NdVar Lo = S.makeTemp(1);
    S.emit(NdOp::INT_AND, Lo, {OldAL, NdVar::cst(0x0F, 1)});
    NdVar LoGt9 = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, LoGt9, {NdVar::cst(9, 1), Lo});
    NdVar Cond1 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, Cond1, {LoGt9, Af});
    NdVar Al6 = S.makeTemp(1);
    S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Al6,
           {OldAL, NdVar::cst(6, 1)});
    NdVar Al1 = S.makeTemp(1);
    S.emit(NdOp::SELECT, Al1, {Cond1, Al6, OldAL});
    // cond2 = (AL > 0x99) || CF  -> high adjust by ±0x60
    NdVar Gt99 = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, Gt99, {NdVar::cst(0x99, 1), OldAL});
    NdVar Cond2 = S.makeTemp(1);
    S.emit(NdOp::INT_OR, Cond2, {Gt99, Cf});
    NdVar Al60 = S.makeTemp(1);
    S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Al60,
           {Al1, NdVar::cst(0x60, 1)});
    NdVar AlNew = S.makeTemp(1);
    S.emit(NdOp::SELECT, AlNew, {Cond2, Al60, Al1});
    // CF: DAS adds the nibble-borrow term (AL<6 || CF) gated by cond1.
    NdVar CfNew = Cond2;
    if (IsSub) {
      NdVar AlLt6 = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, AlLt6, {OldAL, NdVar::cst(6, 1)});
      NdVar Borrow = S.makeTemp(1);
      S.emit(NdOp::INT_OR, Borrow, {AlLt6, Cf});
      NdVar Cf1 = S.makeTemp(1);
      S.emit(NdOp::INT_AND, Cf1, {Cond1, Borrow});
      CfNew = S.makeTemp(1);
      S.emit(NdOp::INT_OR, CfNew, {Cf1, Cond2});
    }
    S.emit(NdOp::COPY, AL, {AlNew});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::AF, 1), {Cond1});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfNew});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    L.emitZSPF(S, AlNew);
    break;
  }
  case X86_INS_AAM:
  case X86_INS_AAD: {
    bool IsMul = (InsnId == X86_INS_AAM);
    uint64_t Base = 10; // implicit base; aam/aad imm8 overrides it
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM)
      Base = static_cast<uint64_t>(X86.operands[0].imm) & 0xFF;
    if (Base == 0)
      Base = 1; // aam/aad 0 faults (#DE); avoid an IR divide-by-zero
    NdVar AL = NdVar::reg(x86reg::RAX, 1);
    NdVar AH = NdVar::reg(x86reg::RAX + 1, 1);
    NdVar OldAL = S.makeTemp(1), OldAH = S.makeTemp(1);
    S.emit(NdOp::COPY, OldAL, {AL});
    S.emit(NdOp::COPY, OldAH, {AH});
    NdVar AlNew = S.makeTemp(1), AhNew = S.makeTemp(1);
    if (IsMul) {
      S.emit(NdOp::INT_DIV, AhNew, {OldAL, NdVar::cst(Base, 1)});
      S.emit(NdOp::INT_REM, AlNew, {OldAL, NdVar::cst(Base, 1)});
    } else {
      NdVar Prod = S.makeTemp(1);
      S.emit(NdOp::INT_MULT, Prod, {OldAH, NdVar::cst(Base, 1)});
      S.emit(NdOp::INT_ADD, AlNew, {Prod, OldAL});
      S.emit(NdOp::COPY, AhNew, {NdVar::cst(0, 1)});
    }
    S.emit(NdOp::COPY, AL, {AlNew});
    S.emit(NdOp::COPY, AH, {AhNew});
    // QEMU sets LOGICB flags: SF/ZF/PF from AL, CF/OF/AF cleared.
    L.emitZSPF(S, AlNew);
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::AF, 1), {NdVar::cst(0, 1)});
    break;
  }

  // BOUND — legacy range check → #BR exception (32-bit only).  Capture the
  // index register and the bounds-pair memory base so codegen can re-emit
  // `bound idx, (base)`.
  case X86_INS_BOUND:
    if (X86.op_count >= 2 && X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_MEM) {
      if (S.memoryAddressSpace(X86.operands[1]) !=
          NdMemoryAddressSpace::Default)
        return false;
      auto Idx = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      uint16_t Sz = static_cast<uint16_t>(X86.operands[0].size);
      if (Sz != 2 && Sz != 4)
        Sz = 4;
      S.emitIntrinsic(Intrinsic::Bound, NdVar(),
                      {NdVar::reg(Idx.Offset, Sz),
                       S.computeEA(X86.operands[1])});
    } else {
      S.emitIntrinsic(Intrinsic::Bound);
    }
    break;
  case X86_INS_INTO:
    S.emitIntrinsic(Intrinsic::Into);
    break;

  // SALC — set AL from carry (undocumented but used).
  // AL = CF ? 0xFF : 0x00
  case X86_INS_SALC: {
    NdVar Al = NdVar::reg(x86reg::RAX, 1);
    NdVar CfExt = S.makeTemp(1);
    S.emit(NdOp::INT_NEG2, CfExt, {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::COPY, Al, {CfExt});
    break;
  }

  // LDS/LES/LSS/LFS/LGS — load far pointer. Approximate as COPY.
  case X86_INS_LDS:
  case X86_INS_LES:
  case X86_INS_LSS:
  case X86_INS_LFS:
  case X86_INS_LGS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
