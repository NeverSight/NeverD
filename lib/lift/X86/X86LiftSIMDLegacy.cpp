//===- X86LiftSIMDLegacy.cpp - x86/x64 legacy/extension instruction lifter ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Legacy, vendor-specific, and extension instruction handlers: BCD, 3DNow!,
/// TBM, CET, VIA PadLock, GFNI, LWP, AMD XOP, SSE4a, and remaining
/// SSE/MMX/system instructions for x86/x64.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftSIMDLegacy(LiftState &S, const cs_insn *Insn,
                               const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // P2: Remaining instructions — BCD, legacy, 3DNow!, TBM, CET, VIA,
  //     x87 misc, LWP, GFNI, system, xsave64, misc new extensions.
  // ========================================================================

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
    emitZSPF(S, AlNew);
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
    emitZSPF(S, AlNew);
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
        X86.operands[1].type == X86_OP_MEM &&
        X86.operands[1].mem.base != X86_REG_INVALID) {
      auto Idx = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      auto Base =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].mem.base));
      uint16_t Sz = static_cast<uint16_t>(X86.operands[0].size);
      if (Sz != 2 && Sz != 4)
        Sz = 4;
      S.emitIntrinsic(
          Intrinsic::Bound, NdVar::reg(x86reg::RAX, 8),
          {NdVar::reg(Idx.Offset, Sz), NdVar::reg(Base.Offset, 4)});
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // 3DNow! — AMD's deprecated SIMD (PF* prefix). Approximate as FLOAT_*.
  // ========================================================================
  case X86_INS_PFADD:
  case X86_INS_PFACC:
  case X86_INS_PFNACC:
  case X86_INS_PFPNACC: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFSUB:
  case X86_INS_PFSUBR: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_SUB, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFMUL: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_MULT, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFMAX:
  case X86_INS_PFMIN: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case X86_INS_PFCMPEQ:
  case X86_INS_PFCMPGE:
  case X86_INS_PFCMPGT: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_EQUAL, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFRCP:
  case X86_INS_PFRCPIT1:
  case X86_INS_PFRCPIT2:
  case X86_INS_PFRSQIT1:
  case X86_INS_PFRSQRT: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_SQRT, Dst, {Src});
    break;
  }
  case X86_INS_PF2ID:
  case X86_INS_PF2IW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }
  case X86_INS_PI2FD:
  case X86_INS_PI2FW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
    break;
  }
  case X86_INS_PSWAPD:
  case X86_INS_PMULHRW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // TBM — trailing bit manipulation (AMD). INT_AND/OR/XOR approximations.
  // ========================================================================
  case X86_INS_BLCFILL:
  case X86_INS_BLCI:
  case X86_INS_BLCIC:
  case X86_INS_BLCMSK:
  case X86_INS_BLCS:
  case X86_INS_BLSFILL:
  case X86_INS_BLSIC:
  case X86_INS_T1MSKC:
  case X86_INS_TZMSK: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    // All TBM ops are of the form: Dst = f(Src, Src±1). Approximate as
    // the dominant operation pattern (AND/OR with adjacents).
    switch (InsnId) {
    case X86_INS_BLCFILL: {
      // Dst = Src & (Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_AND, Dst, {Src, Inc});
      break;
    }
    case X86_INS_BLCI: {
      // Dst = Src | ~(Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      NdVar NotInc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotInc, {Inc});
      S.emit(NdOp::INT_OR, Dst, {Src, NotInc});
      break;
    }
    case X86_INS_BLCIC: {
      // Dst = ~Src & (Src + 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_AND, Dst, {NotSrc, Inc});
      break;
    }
    case X86_INS_BLCMSK: {
      // Dst = Src ^ (Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_XOR, Dst, {Src, Inc});
      break;
    }
    case X86_INS_BLCS: {
      // Dst = Src | (Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {Src, Inc});
      break;
    }
    case X86_INS_BLSFILL: {
      // Dst = Src | (Src - 1)
      NdVar Dec = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {Src, Dec});
      break;
    }
    case X86_INS_BLSIC: {
      // Dst = ~Src | (Src - 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Dec = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {NotSrc, Dec});
      break;
    }
    case X86_INS_T1MSKC: {
      // Dst = ~Src | (Src + 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {NotSrc, Inc});
      break;
    }
    case X86_INS_TZMSK: {
      // Dst = ~Src & (Src - 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Dec = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_AND, Dst, {NotSrc, Dec});
      break;
    }
    default:
      break;
    }
    break;
  }

  // ========================================================================
  // CET — Control-flow Enforcement (ENDBR, shadow stack, etc.).
  // ========================================================================
  case X86_INS_ENDBR32:
  case X86_INS_ENDBR64:
    S.emit(NdOp::NOP, {}, {});
    break;

  case X86_INS_INCSSPD:
  case X86_INS_INCSSPQ:
    S.emitIntrinsic(Intrinsic::CetIncSsp);
    break;
  case X86_INS_RDSSPD:
  case X86_INS_RDSSPQ:
    S.emitIntrinsic(Intrinsic::CetRdSsp);
    if (X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emit(NdOp::COPY, Dst, {S.makeTemp(Dst.Size)});
    }
    break;
  case X86_INS_SAVEPREVSSP:
    S.emitIntrinsic(Intrinsic::CetSaveprevssp);
    break;
  case X86_INS_RSTORSSP:
    S.emitIntrinsic(Intrinsic::CetRstorssp);
    break;
  case X86_INS_WRSSD:
  case X86_INS_WRSSQ:
    S.emitIntrinsic(Intrinsic::CetWrss);
    break;
  case X86_INS_WRUSSD:
  case X86_INS_WRUSSQ:
    S.emitIntrinsic(Intrinsic::CetWruss);
    break;
  case X86_INS_SETSSBSY:
    S.emitIntrinsic(Intrinsic::CetSetssbsy);
    break;
  case X86_INS_CLRSSBSY:
    S.emitIntrinsic(Intrinsic::CetClrssbsy);
    break;

  // MOVDIRI / MOVDIR64B — direct store.
  case X86_INS_MOVDIRI:
  case X86_INS_MOVDIR64B: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Src);
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // PT — Processor Trace.
  case X86_INS_PTWRITE:
    S.emitIntrinsic(Intrinsic::Ptwrite);
    break;

  // ========================================================================
  // VIA PadLock — hardware crypto acceleration (XCRYPT*, XSHA*, MONTMUL,
  // XSTORE).
  // ========================================================================
  case X86_INS_XCRYPTECB:
  case X86_INS_XCRYPTCBC:
  case X86_INS_XCRYPTCTR:
  case X86_INS_XCRYPTCFB:
  case X86_INS_XCRYPTOFB:
  case X86_INS_XSHA1:
  case X86_INS_XSHA256:
  case X86_INS_MONTMUL:
  case X86_INS_XSTORE: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_XCRYPTECB:
      Id = Intrinsic::Xcryptecb;
      break;
    case X86_INS_XCRYPTCBC:
      Id = Intrinsic::Xcryptcbc;
      break;
    case X86_INS_XCRYPTCTR:
      Id = Intrinsic::Xcryptctr;
      break;
    case X86_INS_XCRYPTCFB:
      Id = Intrinsic::Xcryptcfb;
      break;
    case X86_INS_XCRYPTOFB:
      Id = Intrinsic::Xcryptofb;
      break;
    case X86_INS_XSHA1:
      Id = Intrinsic::Xsha1;
      break;
    case X86_INS_XSHA256:
      Id = Intrinsic::Xsha256;
      break;
    case X86_INS_MONTMUL:
      Id = Intrinsic::Montmul;
      break;
    case X86_INS_XSTORE:
      Id = Intrinsic::Xstore;
      break;
    default:
      Id = Intrinsic::Xcryptecb;
      break;
    }
    S.emitIntrinsic(Id);
    for (uint64_t RO : {x86reg::RSI, x86reg::RDI, x86reg::RCX}) {
      S.emit(NdOp::COPY, NdVar::reg(RO, 8), {S.makeTemp(8)});
    }
    break;
  }

  // ========================================================================
  // xsave 64-bit variants.
  // ========================================================================
  case X86_INS_XSAVE64:
    S.emitIntrinsic(Intrinsic::Xsave64);
    break;
  case X86_INS_XRSTOR64:
    S.emitIntrinsic(Intrinsic::Xrstor64);
    break;
  case X86_INS_XSAVES64:
    S.emitIntrinsic(Intrinsic::Xsaves64);
    break;
  case X86_INS_XRSTORS64:
    S.emitIntrinsic(Intrinsic::Xrstors64);
    break;
  case X86_INS_XSAVEC64:
    S.emitIntrinsic(Intrinsic::Xsavec64);
    break;
  case X86_INS_XSAVEOPT64:
    S.emitIntrinsic(Intrinsic::Xsaveopt64);
    break;

  // x87 misc — FNINIT/FNCLEX already handled above in x87 block.

  // ========================================================================
  // GFNI — Galois Field instructions.
  // ========================================================================
  case X86_INS_GF2P8AFFINEINVQB:
  case X86_INS_GF2P8AFFINEQB:
  case X86_INS_GF2P8MULB:
  case X86_INS_VGF2P8AFFINEINVQB:
  case X86_INS_VGF2P8AFFINEQB:
  case X86_INS_VGF2P8MULB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // LWP — Lightweight Profiling (AMD).
  // ========================================================================
  case X86_INS_LLWPCB:
    S.emitIntrinsic(Intrinsic::Llwpcb);
    break;
  case X86_INS_SLWPCB:
    S.emitIntrinsic(Intrinsic::Slwpcb);
    break;
  case X86_INS_LWPINS:
    S.emitIntrinsic(Intrinsic::Lwpins);
    break;
  case X86_INS_LWPVAL:
    S.emitIntrinsic(Intrinsic::Lwpval);
    break;

  // CLAC / STAC — supervisor mode access control.
  case X86_INS_CLAC:
    S.emitIntrinsic(Intrinsic::Clac);
    break;
  case X86_INS_STAC:
    S.emitIntrinsic(Intrinsic::Stac);
    break;

  // CLZERO — zero cache line (AMD Zen).
  case X86_INS_CLZERO:
    S.emitIntrinsic(Intrinsic::Clzero);
    break;

  // XTEST already handled in TSX block.

  // VPCLMUL variants that are not the base VPCLMULQDQ are rare but exist
  // in some Capstone builds. Already covered via VPCLMULQDQ above.

  // ========================================================================
  // Prefixes — not standalone instructions; emit NOP.
  // ========================================================================
  case X86_INS_DATA16:
  case X86_INS_LOCK:
  case X86_INS_REP:
  case X86_INS_REPNE:
  case X86_INS_REX64:
    S.emit(NdOp::NOP, {}, {});
    break;

  // Legacy FPU handled in x87 block above.
  case X86_INS_FCMOVNP:
    S.emitIntrinsic(Intrinsic::X87Op);
    break;

  // ========================================================================
  // SSE/MMX data moves — treat as COPY to preserve dataflow.
  // ========================================================================
  case X86_INS_MOVDQ2Q:
  case X86_INS_MOVQ2DQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case X86_INS_PSHUFW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pshufw, Dst, {Src});
    break;
  }

  // ========================================================================
  // SSE4a (AMD): EXTRQ / INSERTQ → intrinsic (hard to model per-bit).
  // ========================================================================
  case X86_INS_EXTRQ:
    S.emitIntrinsic(Intrinsic::Extrq);
    break;
  case X86_INS_INSERTQ:
    S.emitIntrinsic(Intrinsic::Insertq);
    break;

  // ========================================================================
  // 3DNow! — rare; COPY-based dataflow preservation.
  // ========================================================================
  case X86_INS_PAVGUSB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // System instructions.
  // ========================================================================
  case X86_INS_CLDEMOTE:
    S.emitIntrinsic(Intrinsic::Cldemote);
    break;
  case X86_INS_INVLPGA:
    S.emitIntrinsic(Intrinsic::Invlpga);
    break;
  case X86_INS_PCONFIG:
    S.emitIntrinsic(Intrinsic::Pconfig);
    break;
  case X86_INS_RDPKRU:
    S.emitIntrinsic(Intrinsic::Rdpkru);
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 4), {S.makeTemp(4)});
    break;
  case X86_INS_WRPKRU:
    S.emitIntrinsic(Intrinsic::Wrpkru);
    break;
  case X86_INS_SYSEXITQ:
    S.emitIntrinsic(Intrinsic::Sysexitq);
    break;
  case X86_INS_SYSRETQ:
    S.emitIntrinsic(Intrinsic::Sysretq);
    break;
  case X86_INS_WBNOINVD:
    S.emitIntrinsic(Intrinsic::Wbnoinvd);
    break;

  // ========================================================================
  // AVX/AVX-512 control: VLDMXCSR / VSTMXCSR.
  // ========================================================================
  case X86_INS_VLDMXCSR:
    S.emitIntrinsic(Intrinsic::Ldmxcsr);
    break;
  case X86_INS_VSTMXCSR:
    S.emitIntrinsic(Intrinsic::Stmxcsr);
    break;

  // ========================================================================
  // AVX-512 VPANDN — ~Dst & Src (bulk 128/256/512b).
  // ========================================================================
  case X86_INS_VPANDN: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar Inv = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, Inv, {A});
    S.emit(NdOp::INT_AND, Dst, {Inv, B});
    break;
  }

  // ========================================================================
  // AVX-512 shuffles / blends / aligns — COPY-based dataflow.
  // ========================================================================
  case X86_INS_VALIGND:
  case X86_INS_VALIGNQ:
  case X86_INS_VSHUFF32X4:
  case X86_INS_VSHUFF64X2:
  case X86_INS_VSHUFI32X4:
  case X86_INS_VSHUFI64X2: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // AVX VPCMPESTRI/M, VPCMPISTRI/M → intrinsic (sets RCX + EFLAGS).
  // ========================================================================
  case X86_INS_VPCMPESTRI:
    S.emitIntrinsic(Intrinsic::Pcmpestri);
    break;
  case X86_INS_VPCMPESTRM:
    S.emitIntrinsic(Intrinsic::Pcmpestrm);
    break;
  case X86_INS_VPCMPISTRI:
    S.emitIntrinsic(Intrinsic::Pcmpistri);
    break;
  case X86_INS_VPCMPISTRM:
    S.emitIntrinsic(Intrinsic::Pcmpistrm);
    break;

  // ========================================================================
  // AVX-512 VTESTPD/PS — set ZF/CF from AND/ANDN of packed floats.
  // ========================================================================
  case X86_INS_VTESTPD:
  case X86_INS_VTESTPS: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar AndR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndR, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {AndR, NdVar::cst(0, AndR.Size)});
    NdVar InvA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, InvA, {A});
    NdVar AndnR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndnR, {InvA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndnR, NdVar::cst(0, AndnR.Size)});
    break;
  }

  // ========================================================================
  // AVX-512 VP4DPWSSD / VP4DPWSSDS — dot product accumulate.
  // ========================================================================
  case X86_INS_VP4DPWSSD:
  case X86_INS_VP4DPWSSDS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_ADD, Dst, {Dst, Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPCOM* (integer compare) — Result is all-1s or all-0s Mask.
  // ========================================================================
  case X86_INS_VPCOM:
  case X86_INS_VPCOMB:
  case X86_INS_VPCOMD:
  case X86_INS_VPCOMQ:
  case X86_INS_VPCOMUB:
  case X86_INS_VPCOMUD:
  case X86_INS_VPCOMUQ:
  case X86_INS_VPCOMUW:
  case X86_INS_VPCOMW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    break;
  }

  // ========================================================================
  // AMD XOP: VPCMOV — conditional move; VPERMIL2PD/PS — permute.
  // ========================================================================
  case X86_INS_VPCMOV:
  case X86_INS_VPERMIL2PD:
  case X86_INS_VPERMIL2PS:
  case X86_INS_VPPERM: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPHADD* / VPHSUB* — horizontal add/sub variants.
  // ========================================================================
  case X86_INS_VPHADDBD:
  case X86_INS_VPHADDBQ:
  case X86_INS_VPHADDBW:
  case X86_INS_VPHADDDQ:
  case X86_INS_VPHADDUBD:
  case X86_INS_VPHADDUBQ:
  case X86_INS_VPHADDUBW:
  case X86_INS_VPHADDUDQ:
  case X86_INS_VPHADDUWD:
  case X86_INS_VPHADDUWQ:
  case X86_INS_VPHADDWD:
  case X86_INS_VPHADDWQ:
  case X86_INS_VPHSUBBW:
  case X86_INS_VPHSUBDQ:
  case X86_INS_VPHSUBWD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_ADD, Dst, {Dst, Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPMACS* / VPMADCS* — multiply-accumulate.
  // ========================================================================
  case X86_INS_VPMACSDD:
  case X86_INS_VPMACSDQH:
  case X86_INS_VPMACSDQL:
  case X86_INS_VPMACSSDD:
  case X86_INS_VPMACSSDQH:
  case X86_INS_VPMACSSDQL:
  case X86_INS_VPMACSSWD:
  case X86_INS_VPMACSSWW:
  case X86_INS_VPMACSWD:
  case X86_INS_VPMACSWW:
  case X86_INS_VPMADCSSWD:
  case X86_INS_VPMADCSWD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    break;
  }

  // ========================================================================
  // AMD XOP: VPROT* — packed rotate; VPSHA/VPSHL* — packed shift.
  // ========================================================================
  case X86_INS_VPROTB:
  case X86_INS_VPROTD:
  case X86_INS_VPROTQ:
  case X86_INS_VPROTW:
  case X86_INS_VPSHAB:
  case X86_INS_VPSHAD:
  case X86_INS_VPSHAQ:
  case X86_INS_VPSHAW:
  case X86_INS_VPSHLB:
  case X86_INS_VPSHLD:
  case X86_INS_VPSHLQ:
  case X86_INS_VPSHLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {A, B});
    break;
  }

  // ========================================================================
  // AMD XOP: VFRCZ* — approximate reciprocal.
  // ========================================================================
  case X86_INS_VFRCZPD:
  case X86_INS_VFRCZPS:
  case X86_INS_VFRCZSD:
  case X86_INS_VFRCZSS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // Legacy SSE element-wise integer ops (bulk 128-bit nd-var operations).
  // Loses per-lane semantics but preserves dataflow shape.
  // ========================================================================
  case X86_INS_XORPS:
  case X86_INS_XORPD:
  case X86_INS_PXOR:
  case X86_INS_ORPS:
  case X86_INS_ORPD:
  case X86_INS_POR:
  case X86_INS_ANDPS:
  case X86_INS_ANDPD:
  case X86_INS_PAND:
  case X86_INS_ANDNPS:
  case X86_INS_ANDNPD:
  case X86_INS_PANDN:
  case X86_INS_PADDB:
  case X86_INS_PADDW:
  case X86_INS_PADDD:
  case X86_INS_PADDQ:
  case X86_INS_PSUBB:
  case X86_INS_PSUBW:
  case X86_INS_PSUBD:
  case X86_INS_PSUBQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdOp Opc = NdOp::COPY;
    switch (InsnId) {
    case X86_INS_XORPS:
    case X86_INS_XORPD:
    case X86_INS_PXOR:
      Opc = NdOp::INT_XOR;
      break;
    case X86_INS_ORPS:
    case X86_INS_ORPD:
    case X86_INS_POR:
      Opc = NdOp::INT_OR;
      break;
    case X86_INS_ANDPS:
    case X86_INS_ANDPD:
    case X86_INS_PAND:
      Opc = NdOp::INT_AND;
      break;
    case X86_INS_ANDNPS:
    case X86_INS_ANDNPD:
    case X86_INS_PANDN: {
      NdVar Neg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, Neg, {Dst});
      S.emit(NdOp::INT_AND, Dst, {Neg, Src});
      break;
    }
    case X86_INS_PADDB:
    case X86_INS_PADDW:
    case X86_INS_PADDD:
    case X86_INS_PADDQ:
    case X86_INS_PSUBB:
    case X86_INS_PSUBW:
    case X86_INS_PSUBD:
    case X86_INS_PSUBQ: {
      unsigned LaneSz = 0;
      switch (InsnId) {
      case X86_INS_PADDB:
      case X86_INS_PSUBB:
        LaneSz = 1;
        break;
      case X86_INS_PADDW:
      case X86_INS_PSUBW:
        LaneSz = 2;
        break;
      case X86_INS_PADDD:
      case X86_INS_PSUBD:
        LaneSz = 4;
        break;
      case X86_INS_PADDQ:
      case X86_INS_PSUBQ:
        LaneSz = 8;
        break;
      default:
        break;
      }
      NdOp LaneOpc = (InsnId == X86_INS_PADDB || InsnId == X86_INS_PADDW ||
                      InsnId == X86_INS_PADDD || InsnId == X86_INS_PADDQ)
                         ? NdOp::INT_ADD
                         : NdOp::INT_SUB;
      // Split into low/high halves to avoid non-power-of-2 intermediate types.
      unsigned HalfSz = Dst.Size / 2;
      unsigned LanesPerHalf = HalfSz / LaneSz;
      auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < LanesPerHalf; ++I) {
          unsigned Off = BaseOff + I * LaneSz;
          NdVar La = S.makeTemp(LaneSz);
          NdVar Lb = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Dst, NdVar::cst(Off, 4)});
          S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(LaneOpc, Lr, {La, Lb});
          if (I == 0) {
            Acc = Lr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Lr, Acc});
            Acc = Next;
          }
        }
        return Acc;
      };
      NdVar LoHalf = BuildHalf(0);
      NdVar HiHalf = BuildHalf(HalfSz);
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
      S.emit(NdOp::COPY, Dst, {Full});
      break;
    }
    default:
      Opc = NdOp::COPY;
    }
    if (InsnId != X86_INS_ANDNPS && InsnId != X86_INS_ANDNPD &&
        InsnId != X86_INS_PANDN && InsnId != X86_INS_PADDB &&
        InsnId != X86_INS_PADDW && InsnId != X86_INS_PADDD &&
        InsnId != X86_INS_PADDQ && InsnId != X86_INS_PSUBB &&
        InsnId != X86_INS_PSUBW && InsnId != X86_INS_PSUBD &&
        InsnId != X86_INS_PSUBQ) {
      S.emit(Opc, Dst, {Dst, Src});
    }
    break;
  }

  // ========================================================================
  // PSLLDQ / PSRLDQ (byte-shift) and packed element shifts
  // ========================================================================
  case X86_INS_PSLLDQ:
  case X86_INS_PSRLDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Bits = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_LEFT, Bits, {Src, NdVar::cst(3, Dst.Size)});
    NdOp Opc = (InsnId == X86_INS_PSLLDQ) ? NdOp::INT_LEFT : NdOp::INT_RIGHT;
    S.emit(Opc, Dst, {Dst, Bits});
    break;
  }
  case X86_INS_PSLLD:
  case X86_INS_PSLLW:
  case X86_INS_PSLLQ:
  case X86_INS_PSRLD:
  case X86_INS_PSRLW:
  case X86_INS_PSRLQ:
  case X86_INS_PSRAW:
  case X86_INS_PSRAD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    unsigned LaneSz = 0;
    NdOp ShiftOp = NdOp::INT_LEFT;
    switch (InsnId) {
    case X86_INS_PSLLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSLLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSLLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSRLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRAW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_ASHR;
      break;
    default:
      LaneSz = 4;
      ShiftOp = NdOp::INT_ASHR;
      break;
    }
    unsigned LaneBits = LaneSz * 8;
    bool Arith = (ShiftOp == NdOp::INT_ASHR);

    // The shift count is the scalar in the low 64 bits of the source (or imm8),
    // shared by all lanes.  x86 does NOT mask it: a logical shift yields 0 and
    // an arithmetic shift yields a sign fill when count >= lane bit width.
    NdVar RawCnt;
    if (X86.operands[1].type == X86_OP_IMM) {
      RawCnt = NdVar::cst((uint64_t)X86.operands[1].imm, 8);
    } else {
      NdVar SrcFull = operandRead(S, X86.operands[1]);
      RawCnt = S.makeTemp(8);
      if (SrcFull.Size >= 8)
        S.emit(NdOp::SUBBYTES, RawCnt, {SrcFull, NdVar::cst(0, 4)});
      else
        S.emit(NdOp::INT_ZEXT, RawCnt, {SrcFull});
    }
    NdVar InRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, InRange, {RawCnt, NdVar::cst(LaneBits, 8)});
    NdVar CntL = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, CntL, {RawCnt, NdVar::cst(0, 4)});
    if (Arith) {
      NdVar Clamped = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Clamped,
             {InRange, CntL, NdVar::cst(LaneBits - 1, LaneSz)});
      CntL = Clamped;
    }

    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Dst, NdVar::cst(I * LaneSz, 4)});
      NdVar Raw = S.makeTemp(LaneSz);
      S.emit(ShiftOp, Raw, {Lane, CntL});
      NdVar Shifted = Raw;
      if (!Arith) {
        Shifted = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Shifted, {InRange, Raw, NdVar::cst(0, LaneSz)});
      }
      if (I == 0) {
        Acc = Shifted;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // ========================================================================
  // SIMD shuffles / unpacks
  // ========================================================================
  case X86_INS_PSHUFD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufd, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PSHUFB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pshufb, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PSHUFLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshuflw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PSHUFHW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufhw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHUFPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Shufps, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHUFPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Shufpd, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PUNPCKLBW:
  case X86_INS_PUNPCKHBW:
  case X86_INS_PUNPCKLWD:
  case X86_INS_PUNPCKHWD:
  case X86_INS_PUNPCKLDQ:
  case X86_INS_PUNPCKHDQ:
  case X86_INS_PUNPCKLQDQ:
  case X86_INS_PUNPCKHQDQ:
  case X86_INS_UNPCKLPS:
  case X86_INS_UNPCKHPS:
  case X86_INS_UNPCKLPD:
  case X86_INS_UNPCKHPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_PUNPCKLBW:
      Id = Intrinsic::Punpcklbw;
      break;
    case X86_INS_PUNPCKHBW:
      Id = Intrinsic::Punpckhbw;
      break;
    case X86_INS_PUNPCKLWD:
      Id = Intrinsic::Punpcklwd;
      break;
    case X86_INS_PUNPCKHWD:
      Id = Intrinsic::Punpckhwd;
      break;
    case X86_INS_PUNPCKLDQ:
      Id = Intrinsic::Punpckldq;
      break;
    case X86_INS_PUNPCKHDQ:
      Id = Intrinsic::Punpckhdq;
      break;
    case X86_INS_PUNPCKLQDQ:
      Id = Intrinsic::Punpcklqdq;
      break;
    case X86_INS_PUNPCKHQDQ:
      Id = Intrinsic::Punpckhqdq;
      break;
    case X86_INS_UNPCKLPS:
      Id = Intrinsic::Unpcklps;
      break;
    case X86_INS_UNPCKHPS:
      Id = Intrinsic::Unpckhps;
      break;
    case X86_INS_UNPCKLPD:
      Id = Intrinsic::Unpcklpd;
      break;
    default:
      Id = Intrinsic::Unpckhpd;
      break;
    }
    S.emitIntrinsic(Id, Dst, {Dst, Src});
    break;
  }

  // ========================================================================
  // SIMD scalar/packed compares
  // ========================================================================
  case X86_INS_PCMPEQB:
  case X86_INS_PCMPEQW:
  case X86_INS_PCMPEQD:
  case X86_INS_PCMPGTB:
  case X86_INS_PCMPGTW:
  case X86_INS_PCMPGTD: {
    if (X86.op_count < 2)
      break;
    NdVar Lhs = operandRead(S, X86.operands[0]);
    NdVar Rhs = operandRead(S, X86.operands[1]);
    NdVar Dst = operandWrite(X86.operands[0]);
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_PCMPEQB:
    case X86_INS_PCMPGTB:
      LaneSz = 1;
      break;
    case X86_INS_PCMPEQW:
    case X86_INS_PCMPGTW:
      LaneSz = 2;
      break;
    default:
      LaneSz = 4;
      break;
    }
    bool IsGT = (InsnId == X86_INS_PCMPGTB || InsnId == X86_INS_PCMPGTW ||
                 InsnId == X86_INS_PCMPGTD);
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildCmpHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {Lhs, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Rhs, NdVar::cst(Off, 4)});
        NdVar Cmp = S.makeTemp(1);
        if (IsGT)
          S.emit(NdOp::INT_SLESS, Cmp, {Lb, La});
        else
          S.emit(NdOp::INT_EQUAL, Cmp, {La, Lb});
        NdVar Mask = S.makeTemp(LaneSz);
        uint64_t AllOnes = (LaneSz == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                         : ((1ULL << (LaneSz * 8)) - 1);
        S.emit(NdOp::SELECT, Mask,
               {Cmp, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Mask;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Mask, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoH = BuildCmpHalf(0);
    NdVar HiH = BuildCmpHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiH, LoH});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_UCOMISS:
  case X86_INS_UCOMISD:
  case X86_INS_COMISS:
  case X86_INS_COMISD: {
    if (X86.op_count < 2)
      break;
    NdVar Lhs = operandRead(S, X86.operands[0]);
    NdVar Rhs = operandRead(S, X86.operands[1]);
    // COMISS/UCOMISS compare the low single (4B), COMISD/UCOMISD the low double
    // (8B).  operandRead hands back the whole XMM for a register operand;
    // extract the scalar so the float emitter infers the right precision — a
    // *SS compare left 16B wide would read 8 bytes as a double (sign bit lands
    // at bit 63), inverting the sign test.
    unsigned ScalarSz =
        (InsnId == X86_INS_UCOMISS || InsnId == X86_INS_COMISS) ? 4 : 8;
    if (Lhs.Size > ScalarSz) {
      NdVar T = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, T, {Lhs, NdVar::cst(0, 4)});
      Lhs = T;
    }
    if (Rhs.Size > ScalarSz) {
      NdVar T = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, T, {Rhs, NdVar::cst(0, 4)});
      Rhs = T;
    }
    // An unordered (NaN) compare sets ZF=PF=CF=1.  Bare ordered relations leave
    // ZF/CF 0 on NaN, which makes the SETA/SETAE ordered idioms (!CF&&!ZF /
    // !CF) wrongly read true, so OR the unordered predicate into ZF and CF.
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {Lhs});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {Rhs});
    NdVar Unord = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Unord, {NanA, NanB});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {Lhs, Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::ZF, 1), {Eq, Unord});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {Lhs, Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {Lt, Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::cst(0, 1)});
    break;
  }

  // ========================================================================
  // SSE scalar float arithmetic
  // ========================================================================
  case X86_INS_ADDSS:
  case X86_INS_ADDSD:
  case X86_INS_ADDPS:
  case X86_INS_ADDPD:
  case X86_INS_SUBSS:
  case X86_INS_SUBSD:
  case X86_INS_SUBPS:
  case X86_INS_SUBPD:
  case X86_INS_MULSS:
  case X86_INS_MULSD:
  case X86_INS_MULPS:
  case X86_INS_MULPD:
  case X86_INS_DIVSS:
  case X86_INS_DIVSD:
  case X86_INS_DIVPS:
  case X86_INS_DIVPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_ADDSS:
    case X86_INS_ADDSD:
    case X86_INS_ADDPS:
    case X86_INS_ADDPD:
      Opc = NdOp::FLOAT_ADD;
      break;
    case X86_INS_SUBSS:
    case X86_INS_SUBSD:
    case X86_INS_SUBPS:
    case X86_INS_SUBPD:
      Opc = NdOp::FLOAT_SUB;
      break;
    case X86_INS_MULSS:
    case X86_INS_MULSD:
    case X86_INS_MULPS:
    case X86_INS_MULPD:
      Opc = NdOp::FLOAT_MULT;
      break;
    default:
      Opc = NdOp::FLOAT_DIV;
    }
    bool IsPacked = (InsnId == X86_INS_ADDPS || InsnId == X86_INS_ADDPD ||
                     InsnId == X86_INS_SUBPS || InsnId == X86_INS_SUBPD ||
                     InsnId == X86_INS_MULPS || InsnId == X86_INS_MULPD ||
                     InsnId == X86_INS_DIVPS || InsnId == X86_INS_DIVPD);
    if (IsPacked && Dst.Size >= 16) {
      bool IsPD = (InsnId == X86_INS_ADDPD || InsnId == X86_INS_SUBPD ||
                   InsnId == X86_INS_MULPD || InsnId == X86_INS_DIVPD);
      unsigned ElemSz = IsPD ? 8 : 4;
      unsigned NLanes = Dst.Size / ElemSz;
      std::vector<NdVar> Lanes(NLanes);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar A = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, A, {Dst, NdVar::cst(I * ElemSz, 4)});
        NdVar B = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, B, {Src, NdVar::cst(I * ElemSz, 4)});
        Lanes[I] = S.makeTemp(ElemSz);
        S.emit(Opc, Lanes[I], {A, B});
      }
      if (NLanes == 2) {
        S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
      } else {
        NdVar Lo = S.makeTemp(ElemSz * 2);
        S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
        NdVar Hi = S.makeTemp(ElemSz * 2);
        S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
        S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
      }
    } else {
      bool IsSS = (InsnId == X86_INS_ADDSS || InsnId == X86_INS_SUBSS ||
                   InsnId == X86_INS_MULSS || InsnId == X86_INS_DIVSS);
      bool IsSD = (InsnId == X86_INS_ADDSD || InsnId == X86_INS_SUBSD ||
                   InsnId == X86_INS_MULSD || InsnId == X86_INS_DIVSD);
      if ((IsSS || IsSD) && Dst.Size > 8) {
        unsigned ScalarSz = IsSS ? 4 : 8;
        NdVar A = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, A, {Dst, NdVar::cst(0, 4)});
        NdVar B = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, B, {Src, NdVar::cst(0, 4)});
        NdVar Res = S.makeTemp(ScalarSz);
        S.emit(Opc, Res, {A, B});
        unsigned HiSz = Dst.Size - ScalarSz;
        NdVar Hi = S.makeTemp(HiSz);
        S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::cst(ScalarSz, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, Res});
      } else {
        S.emit(Opc, Dst, {Dst, Src});
      }
    }
    break;
  }

  // ========================================================================
  // SSE scalar conversions
  // ========================================================================
  case X86_INS_CVTSI2SS:
  case X86_INS_CVTSI2SD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
    break;
  }
  case X86_INS_CVTSS2SI:
  case X86_INS_CVTSD2SI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned FPSz = (Insn->id == X86_INS_CVTSS2SI) ? 4 : 8;
    if (Src.Size > FPSz) {
      NdVar Narrow = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
      Src = Narrow;
    }
    // CVTSS2SI/CVTSD2SI round using MXCSR (default: nearest, ties to even),
    // unlike the truncating CVTTSS2SI/CVTTSD2SI.  Round first, then convert.
    NdVar Rounded = S.makeTemp(FPSz);
    S.emit(NdOp::FLOAT_ROUNDEVEN, Rounded, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Rounded});
    break;
  }
  case X86_INS_CVTTSS2SI:
  case X86_INS_CVTTSD2SI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned FPSz = (Insn->id == X86_INS_CVTTSS2SI) ? 4 : 8;
    if (Src.Size > FPSz) {
      NdVar Narrow = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
      Src = Narrow;
    }
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }

  // ========================================================================
  // MXCSR (SSE control/status register)
  // ========================================================================
  case X86_INS_LDMXCSR:
  case X86_INS_STMXCSR:
    S.emitIntrinsic(InsnId == X86_INS_LDMXCSR ? Intrinsic::Ldmxcsr
                                              : Intrinsic::Stmxcsr);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
