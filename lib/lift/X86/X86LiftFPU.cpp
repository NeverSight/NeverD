//===- X86LiftFPU.cpp - x86/x64 x87 FPU instruction lifter --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x87 floating-point instruction handlers: FLD/FST/FIST, FADD/FSUB/FMUL/FDIV,
/// FCOM/FCOMI, transcendentals, FPU control-word, and x87 save/restore.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

#define ST(i)                                                                  \
  NdVar::reg(x86reg::stReg((FPUTop + (i)) & 7), x86reg::FPURegSize)

// Widen a 64-bit IEEE-754 double bit pattern into an 80-bit x87 register slot.
// The built-in x87 constants (FLD1/FLDPI/...) are stored as doubles; the 80-bit
// register holds the FLOAT2FLOAT-widened value (exact for a later 64-bit
// store).
static void fpuSetFromDoubleBits(X86Lifter::LiftState &S, NdVar Dst,
                                 uint64_t Bits) {
  NdVar D = S.makeTemp(8);
  S.emit(NdOp::COPY, D, {NdVar::cst(Bits, 8)});
  S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {D});
}

// Populate the x87 status word from a compare's predicates so a following
// FNSTSW (commonly fnstsw+sahf: C0->CF, C2->PF, C3->ZF) reads the result.
// C0=lt|unord, C2=unord, C3=eq|unord; C1 and exception/TOP bits stay 0 (only
// C0/C2/C3 are consumed by the compare idioms).
static void emitFpuCompareStatus(X86Lifter::LiftState &S, NdVar Eq,
                                 NdVar Lt, NdVar Unord) {
  NdVar C0 = S.makeTemp(1), C3 = S.makeTemp(1);
  S.emit(NdOp::BOOL_OR, C0, {Lt, Unord});
  S.emit(NdOp::BOOL_OR, C3, {Eq, Unord});
  NdVar W0 = S.makeTemp(2), W2 = S.makeTemp(2), W3 = S.makeTemp(2);
  S.emit(NdOp::INT_ZEXT, W0, {C0});
  S.emit(NdOp::INT_ZEXT, W2, {Unord});
  S.emit(NdOp::INT_ZEXT, W3, {C3});
  S.emit(NdOp::INT_LEFT, W0, {W0, NdVar::cst(x86reg::FPU_SW_C0_BIT, 2)});
  S.emit(NdOp::INT_LEFT, W2, {W2, NdVar::cst(x86reg::FPU_SW_C2_BIT, 2)});
  S.emit(NdOp::INT_LEFT, W3, {W3, NdVar::cst(x86reg::FPU_SW_C3_BIT, 2)});
  NdVar Sw = S.makeTemp(2);
  S.emit(NdOp::INT_OR, Sw, {W0, W2});
  S.emit(NdOp::INT_OR, Sw, {Sw, W3});
  S.emit(NdOp::COPY, NdVar::reg(x86reg::FPU_SW, 2), {Sw});
}

// Recover the destination/source st(i) indices for the register form of an x87
// binary arithmetic op.  Capstone gives Intel order (operands[0]=dst,
// operands[1]=src), e.g. `fmul st(2),st` -> dst=0,src=2 and `faddp st(3),st`
// -> dst=3,src=0.  With fewer explicit operands fall back to the implicit
// forms: pop `faddp` is st1 op= st0; single-operand `fadd st(i)` is st0 op=
// st(i); single-operand pop `faddp st(i)` is st(i) op= st0.
static void fpuArithRegIndices(const cs_x86 &X86, bool IsPop, int &DstIdx,
                               int &SrcIdx) {
  int Idx[2] = {0, 0};
  int N = 0;
  for (int I = 0; I < X86.op_count && N < 2; ++I) {
    if (X86.operands[I].type == X86_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[I].reg));
      Idx[N++] = x86reg::stRegIndex(RI.Offset);
    }
  }
  if (N == 2) {
    DstIdx = Idx[0];
    SrcIdx = Idx[1];
  } else if (N == 1) {
    DstIdx = IsPop ? Idx[0] : 0;
    SrcIdx = IsPop ? 0 : Idx[0];
  } else {
    DstIdx = IsPop ? 1 : 0;
    SrcIdx = IsPop ? 0 : 1;
  }
}

// Emit the register/stack form of an x87 binary arithmetic op.  `Reverse` swaps
// the operands (FSUBR/FDIVR: dst = src op dst); `IsPop` pops st0 afterwards.
static void emitFpuArithReg(X86Lifter::LiftState &S, const cs_x86 &X86,
                            int &FPUTop, NdOp Op, bool Reverse, bool IsPop) {
  int DstIdx, SrcIdx;
  fpuArithRegIndices(X86, IsPop, DstIdx, SrcIdx);
  NdVar Dst = ST(DstIdx);
  NdVar Src = ST(SrcIdx);
  if (Reverse)
    S.emit(Op, Dst, {Src, Dst});
  else
    S.emit(Op, Dst, {Dst, Src});
  if (IsPop)
    FPUTop = (FPUTop + 1) & 7;
}

bool X86Lifter::liftFPU(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_FLD: {
    FPUTop = (FPUTop - 1) & 7;
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG) {
      // fld st(i): push a copy of st(i).  The source is relative to the
      // pre-push top, so after the TOP decrement above it sits at st(i+1) --
      // not the absolute physical slot (wrong whenever TOP != 0).
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      int Idx = x86reg::stRegIndex(RI.Offset);
      S.emit(NdOp::COPY, ST(0), {ST(Idx + 1)});
    } else if (X86.op_count >= 1) {
      NdVar Val = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      // FLD always loads a float (m32fp/m64fp); a narrower operand is an m32fp
      // single that widens to the 64-bit register (FLOAT2FLOAT, not INT2FLOAT —
      // that would reinterpret the float bit pattern as an integer).
      if (Val.Size < x86reg::FPURegSize)
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, FVal, {Val});
      else
        S.emit(NdOp::COPY, FVal, {Val});
      S.emit(NdOp::COPY, ST(0), {FVal});
    }
    break;
  }
  case X86_INS_FILD: {
    FPUTop = (FPUTop - 1) & 7;
    if (X86.op_count >= 1) {
      NdVar IVal = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      S.emit(NdOp::FLOAT_INT2FLOAT, FVal, {IVal});
      S.emit(NdOp::COPY, ST(0), {FVal});
    }
    break;
  }
  case X86_INS_FLD1:
  case X86_INS_FLDZ:
  case X86_INS_FLDPI:
  case X86_INS_FLDL2E:
  case X86_INS_FLDL2T:
  case X86_INS_FLDLG2:
  case X86_INS_FLDLN2: {
    // Push a built-in constant.  Each maps to its correctly-rounded double bit
    // pattern (see x86reg::FPU_CONST_*); the placeholder intrinsic that used to
    // back the transcendental constants pushed garbage onto the stack.
    uint64_t Bits = 0;
    switch (InsnId) {
    case X86_INS_FLD1:
      Bits = x86reg::FPU_CONST_1;
      break;
    case X86_INS_FLDZ:
      Bits = 0;
      break;
    case X86_INS_FLDPI:
      Bits = x86reg::FPU_CONST_PI;
      break;
    case X86_INS_FLDL2E:
      Bits = x86reg::FPU_CONST_L2E;
      break;
    case X86_INS_FLDL2T:
      Bits = x86reg::FPU_CONST_L2T;
      break;
    case X86_INS_FLDLG2:
      Bits = x86reg::FPU_CONST_LG2;
      break;
    case X86_INS_FLDLN2:
      Bits = x86reg::FPU_CONST_LN2;
      break;
    default:
      break;
    }
    FPUTop = (FPUTop - 1) & 7;
    fpuSetFromDoubleBits(S, ST(0), Bits);
    break;
  }
  case X86_INS_FST:
  case X86_INS_FSTP: {
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG) {
      // fst/fstp st(i): copy st(0) to st(i), relative to the current top (the
      // pop, if any, happens after) -- not the absolute physical slot.
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      int Idx = x86reg::stRegIndex(RI.Offset);
      S.emit(NdOp::COPY, ST(Idx), {ST(0)});
    } else if (X86.op_count >= 1) {
      // FST m32fp rounds the 64-bit register down to single precision
      // (FLOAT2FLOAT), not a bit truncation of the low 4 bytes.
      NdVar StVal = ST(0);
      uint16_t MemSz = static_cast<uint16_t>(X86.operands[0].size);
      if (MemSz < x86reg::FPURegSize) {
        NdVar Narrow = S.makeTemp(MemSz);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, Narrow, {StVal});
        StVal = Narrow;
      }
      S.storeToMem(X86.operands[0], StVal);
    }
    if (InsnId == X86_INS_FSTP)
      FPUTop = (FPUTop + 1) & 7;
    break;
  }
  case X86_INS_FIST:
  case X86_INS_FISTP:
  case X86_INS_FISTTP: {
    if (X86.op_count >= 1) {
      NdVar IVal = S.makeTemp(static_cast<uint16_t>(X86.operands[0].size));
      // FIST/FISTP round per the FPU control word RC field; FISTTP always
      // truncates.  FLOAT_FLOAT2INT (fptosi) already truncates toward zero, so
      // RC=toward-zero converts the raw value directly while every other mode
      // rounds via FRNDINT first.  Selecting on RC honors the truncating
      // `(int)x` cast idiom (fnstcw/or 0xC00/fldcw) that an unconditional round
      // would lose — FRNDINT alone uses the runtime mode, left at nearest.
      NdVar Src = ST(0);
      if (InsnId != X86_INS_FISTTP) {
        NdVar RC = S.makeTemp(2);
        S.emit(NdOp::INT_RIGHT, RC,
               {NdVar::reg(x86reg::FPU_CW, 2),
                NdVar::cst(x86reg::FPU_CW_RC_SHIFT, 2)});
        NdVar RCMasked = S.makeTemp(2);
        S.emit(NdOp::INT_AND, RCMasked,
               {RC, NdVar::cst(x86reg::FPU_CW_RC_MASK, 2)});
        NdVar IsTrunc = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsTrunc,
               {RCMasked, NdVar::cst(x86reg::FPU_CW_RC_TRUNCATE, 2)});
        NdVar Rounded = S.makeTemp(Src.Size);
        S.emit(NdOp::FLOAT_ROUNDEVEN, Rounded, {Src});
        NdVar Sel = S.makeTemp(Src.Size);
        S.emit(NdOp::SELECT, Sel, {IsTrunc, Src, Rounded});
        Src = Sel;
      }
      S.emit(NdOp::FLOAT_FLOAT2INT, IVal, {Src});
      S.storeToMem(X86.operands[0], IVal);
    }
    if (InsnId != X86_INS_FIST)
      FPUTop = (FPUTop + 1) & 7;
    break;
  }

  case X86_INS_FADD:
  case X86_INS_FIADD: {
    bool IsPop = (Insn->mnemonic[4] == 'p');
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      NdVar Mem = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      if (Mem.Size < x86reg::FPURegSize) {
        NdOp Cvt = (InsnId == X86_INS_FIADD) ? NdOp::FLOAT_INT2FLOAT
                                             : NdOp::FLOAT_FLOAT2FLOAT;
        S.emit(Cvt, FVal, {Mem});
      } else {
        S.emit(NdOp::COPY, FVal, {Mem});
      }
      S.emit(NdOp::FLOAT_ADD, ST(0), {ST(0), FVal});
    } else {
      emitFpuArithReg(S, X86, FPUTop, NdOp::FLOAT_ADD, /*Reverse=*/false,
                      IsPop);
    }
    break;
  }
  case X86_INS_FSUB:
  case X86_INS_FSUBP:
  case X86_INS_FISUB: {
    bool IsPop = (InsnId == X86_INS_FSUBP);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      NdVar Mem = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      if (Mem.Size < x86reg::FPURegSize) {
        NdOp Cvt = (InsnId == X86_INS_FISUB) ? NdOp::FLOAT_INT2FLOAT
                                             : NdOp::FLOAT_FLOAT2FLOAT;
        S.emit(Cvt, FVal, {Mem});
      } else {
        S.emit(NdOp::COPY, FVal, {Mem});
      }
      S.emit(NdOp::FLOAT_SUB, ST(0), {ST(0), FVal});
    } else {
      emitFpuArithReg(S, X86, FPUTop, NdOp::FLOAT_SUB, /*Reverse=*/false,
                      IsPop);
    }
    break;
  }
  case X86_INS_FSUBR:
  case X86_INS_FSUBRP:
  case X86_INS_FISUBR: {
    bool IsPop = (InsnId == X86_INS_FSUBRP);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      NdVar Mem = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      if (Mem.Size < x86reg::FPURegSize) {
        NdOp Cvt = (InsnId == X86_INS_FISUBR) ? NdOp::FLOAT_INT2FLOAT
                                              : NdOp::FLOAT_FLOAT2FLOAT;
        S.emit(Cvt, FVal, {Mem});
      } else {
        S.emit(NdOp::COPY, FVal, {Mem});
      }
      S.emit(NdOp::FLOAT_SUB, ST(0), {FVal, ST(0)});
    } else {
      emitFpuArithReg(S, X86, FPUTop, NdOp::FLOAT_SUB, /*Reverse=*/true, IsPop);
    }
    break;
  }
  case X86_INS_FMUL:
  case X86_INS_FMULP:
  case X86_INS_FIMUL: {
    bool IsPop = (InsnId == X86_INS_FMULP);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      NdVar Mem = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      if (Mem.Size < x86reg::FPURegSize) {
        NdOp Cvt = (InsnId == X86_INS_FIMUL) ? NdOp::FLOAT_INT2FLOAT
                                             : NdOp::FLOAT_FLOAT2FLOAT;
        S.emit(Cvt, FVal, {Mem});
      } else {
        S.emit(NdOp::COPY, FVal, {Mem});
      }
      S.emit(NdOp::FLOAT_MULT, ST(0), {ST(0), FVal});
    } else {
      emitFpuArithReg(S, X86, FPUTop, NdOp::FLOAT_MULT, /*Reverse=*/false,
                      IsPop);
    }
    break;
  }
  case X86_INS_FDIV:
  case X86_INS_FDIVP:
  case X86_INS_FIDIV: {
    bool IsPop = (InsnId == X86_INS_FDIVP);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      NdVar Mem = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      if (Mem.Size < x86reg::FPURegSize) {
        NdOp Cvt = (InsnId == X86_INS_FIDIV) ? NdOp::FLOAT_INT2FLOAT
                                             : NdOp::FLOAT_FLOAT2FLOAT;
        S.emit(Cvt, FVal, {Mem});
      } else {
        S.emit(NdOp::COPY, FVal, {Mem});
      }
      S.emit(NdOp::FLOAT_DIV, ST(0), {ST(0), FVal});
    } else {
      emitFpuArithReg(S, X86, FPUTop, NdOp::FLOAT_DIV, /*Reverse=*/false,
                      IsPop);
    }
    break;
  }
  case X86_INS_FDIVR:
  case X86_INS_FDIVRP:
  case X86_INS_FIDIVR: {
    bool IsPop = (InsnId == X86_INS_FDIVRP);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      NdVar Mem = operandRead(S, X86.operands[0]);
      NdVar FVal = S.makeTemp(x86reg::FPURegSize);
      if (Mem.Size < x86reg::FPURegSize) {
        NdOp Cvt = (InsnId == X86_INS_FIDIVR) ? NdOp::FLOAT_INT2FLOAT
                                              : NdOp::FLOAT_FLOAT2FLOAT;
        S.emit(Cvt, FVal, {Mem});
      } else {
        S.emit(NdOp::COPY, FVal, {Mem});
      }
      S.emit(NdOp::FLOAT_DIV, ST(0), {FVal, ST(0)});
    } else {
      emitFpuArithReg(S, X86, FPUTop, NdOp::FLOAT_DIV, /*Reverse=*/true, IsPop);
    }
    break;
  }

  case X86_INS_FABS:
    S.emit(NdOp::FLOAT_ABS, ST(0), {ST(0)});
    break;
  case X86_INS_FCHS:
    S.emit(NdOp::FLOAT_NEG, ST(0), {ST(0)});
    break;
  case X86_INS_FSQRT:
    S.emit(NdOp::FLOAT_SQRT, ST(0), {ST(0)});
    break;
  case X86_INS_FRNDINT:
    // Rounds per the FPU control word (default: nearest, ties to even).
    S.emit(NdOp::FLOAT_ROUNDEVEN, ST(0), {ST(0)});
    break;

  case X86_INS_FXCH: {
    int Idx = 1;
    for (int I = 0; I < X86.op_count; ++I) {
      if (X86.operands[I].type == X86_OP_REG) {
        auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[I].reg));
        int CandIdx = x86reg::stRegIndex(RI.Offset);
        if (CandIdx != 0) {
          Idx = CandIdx;
          break;
        }
      }
    }
    NdVar TmpV = S.makeTemp(x86reg::FPURegSize);
    S.emit(NdOp::COPY, TmpV, {ST(0)});
    S.emit(NdOp::COPY, ST(0), {ST(Idx)});
    S.emit(NdOp::COPY, ST(Idx), {TmpV});
    break;
  }

  case X86_INS_FCOM:
  case X86_INS_FCOMP:
  case X86_INS_FCOMPP:
  case X86_INS_FICOM:
  case X86_INS_FICOMP:
  case X86_INS_FUCOM:
  case X86_INS_FUCOMP:
  case X86_INS_FUCOMPP: {
    NdVar Rhs = ST(1);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      Rhs = operandRead(S, X86.operands[0]);
      if (Rhs.Size < x86reg::FPURegSize) {
        NdVar Cvt = S.makeTemp(x86reg::FPURegSize);
        bool IsInt = (InsnId == X86_INS_FICOM || InsnId == X86_INS_FICOMP);
        S.emit(IsInt ? NdOp::FLOAT_INT2FLOAT : NdOp::FLOAT_FLOAT2FLOAT, Cvt,
               {Rhs});
        Rhs = Cvt;
      }
    } else if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG) {
      // fcom/fucom st(i): compare st0 with st(i), relative to the current top.
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      int Idx = x86reg::stRegIndex(RI.Offset);
      Rhs = ST(Idx);
    }
    // An unordered (NaN) compare sets the C3/C2/C0 condition codes (ZF/PF/CF
    // after FNSTSW+SAHF) all to 1; OR the unordered predicate into ZF/CF so the
    // ordered SETcc/Jcc idioms read false on NaN (see UCOMISD).
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {ST(0)});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {Rhs});
    NdVar Unord = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Unord, {NanA, NanB});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {ST(0), Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::ZF, 1), {Eq, Unord});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {ST(0), Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {Lt, Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {Unord});
    // FCOM/FUCOM/FICOM report through the status word, read back by FNSTSW.
    emitFpuCompareStatus(S, Eq, Lt, Unord);
    if (InsnId == X86_INS_FCOMP || InsnId == X86_INS_FICOMP ||
        InsnId == X86_INS_FUCOMP)
      FPUTop = (FPUTop + 1) & 7;
    if (InsnId == X86_INS_FCOMPP || InsnId == X86_INS_FUCOMPP)
      FPUTop = (FPUTop + 2) & 7;
    break;
  }

  case X86_INS_FCOMI:
  case X86_INS_FCOMPI:
  case X86_INS_FUCOMI:
  case X86_INS_FUCOMPI: {
    // The comparand st(i) is whichever ST register operand is not the implicit
    // st0; scan every operand because capstone places it at index 0 (`fcomi
    // st(i)`) or 1 (`fcomi st0, st(i)`) depending on form.  st(i) is relative
    // to the current top, not the absolute physical slot.
    int Idx = 1;
    for (int I = 0; I < X86.op_count; ++I) {
      if (X86.operands[I].type == X86_OP_REG) {
        auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[I].reg));
        int CandIdx = x86reg::stRegIndex(RI.Offset);
        if (CandIdx != 0) {
          Idx = CandIdx;
          break;
        }
      }
    }
    NdVar Rhs = ST(Idx);
    // FUCOMI/FCOMI set EFLAGS directly; an unordered (NaN) compare sets
    // ZF=PF=CF=1.  OR the unordered predicate into ZF/CF so SETA/SETAE (the
    // ordered </<=/>/>= idioms, used by long double compares) read false on
    // NaN.
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {ST(0)});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {Rhs});
    NdVar Unord = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Unord, {NanA, NanB});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {ST(0), Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::ZF, 1), {Eq, Unord});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {ST(0), Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {Lt, Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    if (InsnId == X86_INS_FCOMPI || InsnId == X86_INS_FUCOMPI)
      FPUTop = (FPUTop + 1) & 7;
    break;
  }

  case X86_INS_FTST: {
    NdVar Zero = S.makeTemp(x86reg::FPURegSize);
    S.emit(NdOp::COPY, Zero, {NdVar::cst(0, x86reg::FPURegSize)});
    // ST(0)=NaN is unordered (the other operand 0.0 never is): C3/C2/C0 all 1.
    NdVar Unord = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, Unord, {ST(0)});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {ST(0), Zero});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::ZF, 1), {Eq, Unord});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {ST(0), Zero});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {Lt, Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {Unord});
    emitFpuCompareStatus(S, Eq, Lt, Unord);
    break;
  }

  case X86_INS_FNSTSW: {
    if (X86.op_count >= 1) {
      NdVar Sw = NdVar::reg(x86reg::FPU_SW, 2);
      if (X86.operands[0].type == X86_OP_REG)
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 2), {Sw});
      else
        S.storeToMem(X86.operands[0], Sw);
    }
    break;
  }
  case X86_INS_FNSTCW: {
    if (X86.op_count >= 1)
      S.storeToMem(X86.operands[0], NdVar::reg(x86reg::FPU_CW, 2));
    break;
  }
  case X86_INS_FLDCW: {
    if (X86.op_count >= 1) {
      NdVar Val = operandRead(S, X86.operands[0]);
      S.emit(NdOp::COPY, NdVar::reg(x86reg::FPU_CW, 2), {Val});
    }
    break;
  }

  case X86_INS_FNINIT:
    S.emitVoidIntrinsic(Intrinsic::X87Fninit);
    FPUTop = 0;
    FpuReset = true;
    break;
  case X86_INS_FNCLEX:
    S.emitVoidIntrinsic(Intrinsic::X87Fnclex);
    break;

  // x87 transcendental / special ops: emit the genuine instruction (the backend
  // keeps it as inline asm so it roundtrips through Unicorn unchanged) with the
  // correct x87-stack effect, instead of a value-less X87Op placeholder that
  // the emitter turned into a silent 0 (same family as #303/#377/#381).
  case X86_INS_FSIN:
    S.emitIntrinsic(Intrinsic::X87Fsin, ST(0), {ST(0)});
    break;
  case X86_INS_FCOS:
    S.emitIntrinsic(Intrinsic::X87Fcos, ST(0), {ST(0)});
    break;
  case X86_INS_F2XM1:
    S.emitIntrinsic(Intrinsic::X87F2xm1, ST(0), {ST(0)});
    break;
  // 2-operand, no pop: st0 = f(st0, st1).
  case X86_INS_FSCALE:
    S.emitIntrinsic(Intrinsic::X87Fscale, ST(0), {ST(0), ST(1)});
    break;
  case X86_INS_FPREM:
    S.emitIntrinsic(Intrinsic::X87Fprem, ST(0), {ST(0), ST(1)});
    break;
  case X86_INS_FPREM1:
    S.emitIntrinsic(Intrinsic::X87Fprem1, ST(0), {ST(0), ST(1)});
    break;
  // 2-operand, pop: result lands in st1, then st0 is popped (st1 becomes top).
  case X86_INS_FPATAN:
    S.emitIntrinsic(Intrinsic::X87Fpatan, ST(1), {ST(0), ST(1)});
    FPUTop = (FPUTop + 1) & 7;
    break;
  case X86_INS_FYL2X:
    S.emitIntrinsic(Intrinsic::X87Fyl2x, ST(1), {ST(0), ST(1)});
    FPUTop = (FPUTop + 1) & 7;
    break;
  case X86_INS_FYL2XP1:
    S.emitIntrinsic(Intrinsic::X87Fyl2xp1, ST(1), {ST(0), ST(1)});
    FPUTop = (FPUTop + 1) & 7;
    break;
  // FSINCOS: st0 = sin, push cos.  Hardware fsin/fcos share fsincos's argument
  // reduction, so computing them separately is bit-identical.
  case X86_INS_FSINCOS: {
    NdVar Sin = S.makeTemp(x86reg::FPURegSize);
    NdVar Cos = S.makeTemp(x86reg::FPURegSize);
    S.emitIntrinsic(Intrinsic::X87Fsin, Sin, {ST(0)});
    S.emitIntrinsic(Intrinsic::X87Fcos, Cos, {ST(0)});
    S.emit(NdOp::COPY, ST(0), {Sin});
    FPUTop = (FPUTop - 1) & 7;
    S.emit(NdOp::COPY, ST(0), {Cos});
    break;
  }
  // FPTAN: st0 = tan, push 1.0.
  case X86_INS_FPTAN: {
    NdVar Tan = S.makeTemp(x86reg::FPURegSize);
    S.emitIntrinsic(Intrinsic::X87Fptan, Tan, {ST(0)});
    S.emit(NdOp::COPY, ST(0), {Tan});
    FPUTop = (FPUTop - 1) & 7;
    fpuSetFromDoubleBits(S, ST(0), x86reg::FPU_CONST_1);
    break;
  }
  // FXTRACT: st0 = exponent, push the significand.
  case X86_INS_FXTRACT: {
    NdVar Exp = S.makeTemp(x86reg::FPURegSize);
    NdVar Sig = S.makeTemp(x86reg::FPURegSize);
    S.emitIntrinsic(Intrinsic::X87Fxtractexp, Exp, {ST(0)});
    S.emitIntrinsic(Intrinsic::X87Fxtractsig, Sig, {ST(0)});
    S.emit(NdOp::COPY, ST(0), {Exp});
    FPUTop = (FPUTop - 1) & 7;
    S.emit(NdOp::COPY, ST(0), {Sig});
    break;
  }
  // FXAM classifies st(0) into the status-word condition codes without changing
  // st(0); model as a side effect (no value) so st(0) is preserved.
  case X86_INS_FXAM:
    S.emitIntrinsic(Intrinsic::X87Op);
    break;

  // FCMOVcc: move st(i) into st(0) when the EFLAGS condition (set by a prior
  // FUCOMI/FCOMI) holds.  Previously a no-op placeholder, so the move was lost.
  case X86_INS_FCMOVB:
  case X86_INS_FCMOVBE:
  case X86_INS_FCMOVE:
  case X86_INS_FCMOVU:
  case X86_INS_FCMOVNB:
  case X86_INS_FCMOVNBE:
  case X86_INS_FCMOVNE:
  case X86_INS_FCMOVNU: {
    // Source st(i): the register operand that isn't st0 (mirrors FXCH).
    int Idx = 1;
    for (int I = 0; I < X86.op_count; ++I) {
      if (X86.operands[I].type == X86_OP_REG) {
        auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[I].reg));
        int CandIdx = x86reg::stRegIndex(RI.Offset);
        if (CandIdx != 0) {
          Idx = CandIdx;
          break;
        }
      }
    }
    NdVar Cf = NdVar::reg(x86reg::CF, 1);
    NdVar Zf = NdVar::reg(x86reg::ZF, 1);
    NdVar Pf = NdVar::reg(x86reg::PF, 1);
    NdVar Cond = S.makeTemp(1);
    switch (InsnId) {
    case X86_INS_FCMOVB:
      S.emit(NdOp::COPY, Cond, {Cf});
      break;
    case X86_INS_FCMOVE:
      S.emit(NdOp::COPY, Cond, {Zf});
      break;
    case X86_INS_FCMOVU:
      S.emit(NdOp::COPY, Cond, {Pf});
      break;
    case X86_INS_FCMOVBE:
      S.emit(NdOp::BOOL_OR, Cond, {Cf, Zf});
      break;
    case X86_INS_FCMOVNB:
      S.emit(NdOp::BOOL_NOT, Cond, {Cf});
      break;
    case X86_INS_FCMOVNE:
      S.emit(NdOp::BOOL_NOT, Cond, {Zf});
      break;
    case X86_INS_FCMOVNU:
      S.emit(NdOp::BOOL_NOT, Cond, {Pf});
      break;
    case X86_INS_FCMOVNBE: {
      NdVar Or = S.makeTemp(1);
      S.emit(NdOp::BOOL_OR, Or, {Cf, Zf});
      S.emit(NdOp::BOOL_NOT, Cond, {Or});
      break;
    }
    default:
      break;
    }
    NdVar NewVal = S.makeTemp(x86reg::FPURegSize);
    S.emit(NdOp::SELECT, NewVal, {Cond, ST(Idx), ST(0)});
    S.emit(NdOp::COPY, ST(0), {NewVal});
    break;
  }

  case X86_INS_FNSTENV:
  case X86_INS_FLDENV:
  case X86_INS_FNSAVE:
  case X86_INS_FRSTOR:
  case X86_INS_FXSAVE:
  case X86_INS_FXRSTOR:
  case X86_INS_FXSAVE64:
  case X86_INS_FXRSTOR64:
  case X86_INS_WAIT:
  case X86_INS_FFREE:
  case X86_INS_FFREEP:
  case X86_INS_FDECSTP:
  case X86_INS_FINCSTP:
  case X86_INS_FSTPNCE:
  case X86_INS_FBLD:
  case X86_INS_FBSTP:
  case X86_INS_FSETPM:
  case X86_INS_FDISI8087_NOP:
  case X86_INS_FENI8087_NOP:
    S.emitIntrinsic(Intrinsic::X87Op);
    break;

  default:
    return false;
  }
  return true;
}

#undef ST

} // namespace neverd
