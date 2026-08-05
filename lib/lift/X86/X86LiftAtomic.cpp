//===- X86LiftAtomic.cpp - x86/x64 atomic instruction lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic / lock-prefix instruction handlers for x86/x64: CMPXCHG,
/// CMPXCHG8B, CMPXCHG16B, and XADD.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftAtomic(LiftState &S, const cs_insn *Insn,
                           const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // CMPXCHG Dst, Src: if (RAX == Dst) Dst = Src else RAX = Dst; set ZF
  case X86_INS_CMPXCHG: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint16_t Sz = DstR.Size;
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
    NdVar CmpTmp = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CmpTmp, {Rax, DstR});
    emitFlagsArith(S, CmpTmp, Rax, DstR, true);
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::COPY, Eq, {NdVar::reg(x86reg::ZF, 1)});
    NdVar EqExt = S.makeTemp(Sz);
    S.emit(NdOp::INT_ZEXT, EqExt, {Eq});
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_NEG2, Mask, {EqExt});
    NdVar Inv = S.makeTemp(Sz);
    S.emit(NdOp::INT_NOT, Inv, {Mask});
    // dest = ZF ? Src : dest.  A MEMORY destination needs an explicit STORE —
    // operandWrite() yields a discarded ram(0) placeholder, so the prior code
    // silently dropped the write-back (memory left unchanged even on a match).
    // Store before the RAX update so a RAX-based address operand stays
    // original.
    NdVar T1 = S.makeTemp(Sz);
    NdVar T2 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, T1, {Src, Mask});
    S.emit(NdOp::INT_AND, T2, {DstR, Inv});
    NdVar DstVal = IsMem ? S.makeTemp(Sz) : operandWrite(X86.operands[0]);
    S.emit(NdOp::INT_OR, DstVal, {T1, T2});
    if (IsMem)
      S.storeToMem(X86.operands[0], DstVal);
    NdVar R1 = S.makeTemp(Sz);
    NdVar R2 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, R1, {Rax, Mask});
    S.emit(NdOp::INT_AND, R2, {DstR, Inv});
    S.emit(NdOp::INT_OR, Rax, {R1, R2});
    break;
  }

  // CMPXCHG8B [m64]
  case X86_INS_CMPXCHG8B: {
    if (X86.op_count < 1)
      break;
    NdVar MemR = operandRead(S, X86.operands[0]);
    NdVar EdxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EdxZ, {NdVar::reg(x86reg::RDX, 4)});
    NdVar EdxSh = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, EdxSh, {EdxZ, NdVar::cst(32, 8)});
    NdVar EaxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EaxZ, {NdVar::reg(x86reg::RAX, 4)});
    NdVar DxAx = S.makeTemp(8);
    S.emit(NdOp::INT_OR, DxAx, {EdxSh, EaxZ});
    NdVar EcxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EcxZ, {NdVar::reg(x86reg::RCX, 4)});
    NdVar EcxSh = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, EcxSh, {EcxZ, NdVar::cst(32, 8)});
    NdVar EbxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EbxZ, {NdVar::reg(x86reg::RBX, 4)});
    NdVar CxBx = S.makeTemp(8);
    S.emit(NdOp::INT_OR, CxBx, {EcxSh, EbxZ});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Eq, {MemR, DxAx});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {Eq});
    NdVar EqExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EqExt, {Eq});
    NdVar Mask = S.makeTemp(8);
    S.emit(NdOp::INT_NEG2, Mask, {EqExt});
    NdVar Inv = S.makeTemp(8);
    S.emit(NdOp::INT_NOT, Inv, {Mask});
    NdVar M1 = S.makeTemp(8);
    NdVar M2 = S.makeTemp(8);
    S.emit(NdOp::INT_AND, M1, {CxBx, Mask});
    S.emit(NdOp::INT_AND, M2, {MemR, Inv});
    // Write the (conditionally) updated pair back to memory via an explicit
    // STORE — operandWrite() returns a discarded ram(0) placeholder, so the old
    // code never updated memory.  Store before the EAX:EDX update so an address
    // that uses those registers is still the original.
    NdVar NewMem = S.makeTemp(8);
    S.emit(NdOp::INT_OR, NewMem, {M1, M2});
    S.storeToMem(X86.operands[0], NewMem);
    NdVar MemLo = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, MemLo, {MemR, NdVar::cst(0, 4)});
    NdVar MemHi = S.makeTemp(8);
    S.emit(NdOp::INT_RIGHT, MemHi, {MemR, NdVar::cst(32, 8)});
    NdVar MemHiLo = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, MemHiLo, {MemHi, NdVar::cst(0, 4)});
    NdVar EqExt4 = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, EqExt4, {Eq});
    NdVar Mask4 = S.makeTemp(4);
    S.emit(NdOp::INT_NEG2, Mask4, {EqExt4});
    NdVar Inv4 = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, Inv4, {Mask4});
    NdVar AxK = S.makeTemp(4);
    S.emit(NdOp::INT_AND, AxK, {NdVar::reg(x86reg::RAX, 4), Mask4});
    NdVar AxN = S.makeTemp(4);
    S.emit(NdOp::INT_AND, AxN, {MemLo, Inv4});
    S.emit(NdOp::INT_OR, NdVar::reg(x86reg::RAX, 4), {AxK, AxN});
    NdVar DxK = S.makeTemp(4);
    S.emit(NdOp::INT_AND, DxK, {NdVar::reg(x86reg::RDX, 4), Mask4});
    NdVar DxN = S.makeTemp(4);
    S.emit(NdOp::INT_AND, DxN, {MemHiLo, Inv4});
    S.emit(NdOp::INT_OR, NdVar::reg(x86reg::RDX, 4), {DxK, DxN});
    break;
  }

  // CMPXCHG16B [m128] (x86-64 only)
  case X86_INS_CMPXCHG16B: {
    if (X86.op_count < 1)
      break;
    if (TargetArch != Arch::X64)
      break;
    NdVar MemR = operandRead(S, X86.operands[0]);
    NdVar Rdx = NdVar::reg(x86reg::RDX, 8);
    NdVar Rax = NdVar::reg(x86reg::RAX, 8);
    NdVar Rcx = NdVar::reg(x86reg::RCX, 8);
    NdVar Rbx = NdVar::reg(x86reg::RBX, 8);
    NdVar MemLo64 = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, MemLo64, {MemR, NdVar::cst(0, 8)});
    NdVar MemHi64 = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, MemHi64, {MemR, NdVar::cst(8, 8)});
    NdVar EqLo = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, EqLo, {MemLo64, Rax});
    NdVar EqHi = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, EqHi, {MemHi64, Rdx});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::BOOL_AND, Eq, {EqLo, EqHi});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {Eq});
    NdVar CxBx = S.makeTemp(16);
    S.emit(NdOp::CONCAT, CxBx, {Rcx, Rbx});
    NdVar EqExt = S.makeTemp(16);
    S.emit(NdOp::INT_ZEXT, EqExt, {Eq});
    NdVar Mask = S.makeTemp(16);
    S.emit(NdOp::INT_NEG2, Mask, {EqExt});
    NdVar Inv = S.makeTemp(16);
    S.emit(NdOp::INT_NOT, Inv, {Mask});
    NdVar M1 = S.makeTemp(16);
    NdVar M2 = S.makeTemp(16);
    S.emit(NdOp::INT_AND, M1, {CxBx, Mask});
    S.emit(NdOp::INT_AND, M2, {MemR, Inv});
    // Explicit STORE of the (conditionally) updated 128-bit pair;
    // operandWrite() yields a discarded ram(0) placeholder so the old code
    // never wrote memory. Store before the RAX:RDX update so a register-based
    // address stays original.
    NdVar NewMem = S.makeTemp(16);
    S.emit(NdOp::INT_OR, NewMem, {M1, M2});
    S.storeToMem(X86.operands[0], NewMem);
    NdVar EqExt8 = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EqExt8, {Eq});
    NdVar Mask8 = S.makeTemp(8);
    S.emit(NdOp::INT_NEG2, Mask8, {EqExt8});
    NdVar Inv8 = S.makeTemp(8);
    S.emit(NdOp::INT_NOT, Inv8, {Mask8});
    NdVar RaxK = S.makeTemp(8);
    S.emit(NdOp::INT_AND, RaxK, {Rax, Mask8});
    NdVar RaxN = S.makeTemp(8);
    S.emit(NdOp::INT_AND, RaxN, {MemLo64, Inv8});
    S.emit(NdOp::INT_OR, Rax, {RaxK, RaxN});
    NdVar RdxK = S.makeTemp(8);
    S.emit(NdOp::INT_AND, RdxK, {Rdx, Mask8});
    NdVar RdxN = S.makeTemp(8);
    S.emit(NdOp::INT_AND, RdxN, {MemHi64, Inv8});
    S.emit(NdOp::INT_OR, Rdx, {RdxK, RdxN});
    break;
  }

  // XADD Dst, Src: Tmp = Dst + Src; Src = Dst (old); Dst = Tmp
  case X86_INS_XADD: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar SrcW = operandWrite(X86.operands[1]); // src operand is always a reg
    NdVar SrcR = operandRead(S, X86.operands[1]);
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Sum = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ADD, Sum, {DstR, SrcR});
    // Compute the flags from the original operands BEFORE the register writes:
    // CF/OF/AF read DstR/SrcR, which would otherwise resolve to the post-write
    // values once SrcW=DstR and DstW=Sum land (write-before-snapshot, cf.
    // #309).
    emitFlagsArith(S, Sum, DstR, SrcR, false);
    if (IsMem) {
      // A MEMORY destination needs an explicit STORE — operandWrite() yields a
      // discarded ram(0) placeholder, so the old code dropped the write-back
      // and left memory unchanged.  Store the sum BEFORE clobbering the source
      // register (which may appear in the destination's address operands), then
      // load the old destination into the source register.
      S.storeToMem(X86.operands[0], Sum);
      S.emit(NdOp::COPY, SrcW, {DstR});
    } else {
      NdVar DstW = operandWrite(X86.operands[0]);
      S.emit(NdOp::COPY, SrcW, {DstR});
      S.emit(NdOp::COPY, DstW, {Sum});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
