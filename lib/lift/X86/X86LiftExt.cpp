//===- X86LiftExt.cpp - x86/x64 extension instruction lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Extension instruction handlers for x86/x64: BMI1 (BLSI, BLSMSK, BLSR,
/// ANDN, BEXTR), BMI2 (BZHI, MULX, PDEP, PEXT, RORX, SARX, SHLX, SHRX),
/// ADX (ADCX, ADOX), bit-counting (TZCNT, LZCNT, POPCNT), and MOVBE.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftExt(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Bit counting: TZCNT, LZCNT, POPCNT
  // ========================================================================
  case X86_INS_TZCNT:
  case X86_INS_LZCNT:
  case X86_INS_POPCNT: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Dst = operandWrite(X86.operands[0]);
    if (InsnId == X86_INS_TZCNT) {
      NdVar NotX = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NOT, NotX, {Src});
      NdVar XM1 = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_SUB, XM1, {Src, NdVar::cst(1, Src.Size)});
      NdVar Iso = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_AND, Iso, {NotX, XM1});
      S.emit(NdOp::POPCOUNT, Dst, {Iso});
    } else {
      NdOp Opc = (InsnId == X86_INS_LZCNT) ? NdOp::LZCOUNT : NdOp::POPCOUNT;
      S.emit(Opc, Dst, {Src});
    }
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    if (InsnId == X86_INS_POPCNT) {
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
    } else {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
             {Src, NdVar::cst(0, Src.Size)});
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::cst(0, 1)});
    break;
  }

  // ========================================================================
  // BMI1: BLSI, BLSMSK, BLSR, ANDN, BEXTR
  // ========================================================================
  case X86_INS_BLSI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Neg = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NEG2, Neg, {Src});
    S.emit(NdOp::INT_AND, Dst, {Neg, Src});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {Src, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    break;
  }
  case X86_INS_BLSMSK: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Dec = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
    S.emit(NdOp::INT_XOR, Dst, {Dec, Src});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {Src, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    break;
  }
  case X86_INS_BLSR: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Dec = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
    S.emit(NdOp::INT_AND, Dst, {Dec, Src});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {Src, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    break;
  }
  case X86_INS_ANDN: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::cst(0, Dst.Size)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    break;
  }

  case X86_INS_BEXTR: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Ctrl = operandRead(S, X86.operands[2]);
    uint16_t Sz = Dst.Size;
    NdVar Start = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Start, {Ctrl, NdVar::cst(0xFF, Sz)});
    NdVar Shifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shifted, {Src, Start});
    NdVar Len = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Len, {Ctrl, NdVar::cst(8, Sz)});
    S.emit(NdOp::INT_AND, Len, {Len, NdVar::cst(0xFF, Sz)});
    NdVar One = NdVar::cst(1, Sz);
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Mask, {One, Len});
    S.emit(NdOp::INT_SUB, Mask, {Mask, One});
    S.emit(NdOp::INT_AND, Dst, {Shifted, Mask});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Sz)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    break;
  }

  // ========================================================================
  // BMI2: BZHI, MULX, PDEP, PEXT, RORX, SARX, SHLX, SHRX
  // ========================================================================
  case X86_INS_BZHI: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Idx = operandRead(S, X86.operands[2]);
    uint16_t Sz = Dst.Size;
    NdVar IdxLow = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, IdxLow, {Idx, NdVar::cst(0xFF, Sz)});
    NdVar One = NdVar::cst(1, Sz);
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Mask, {One, IdxLow});
    S.emit(NdOp::INT_SUB, Mask, {Mask, One});
    S.emit(NdOp::INT_AND, Dst, {Src, Mask});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Dst, NdVar::cst(0, Sz)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Dst, NdVar::cst(0, Sz)});
    uint64_t BitWidth = Sz * 8;
    NdVar IdxInRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, IdxInRange, {IdxLow, NdVar::cst(BitWidth, Sz)});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(x86reg::CF, 1), {IdxInRange});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    break;
  }

  case X86_INS_MULX: {
    if (X86.op_count < 3)
      break;
    NdVar DstHi = operandWrite(X86.operands[0]);
    NdVar DstLo = operandWrite(X86.operands[1]);
    NdVar Src = operandRead(S, X86.operands[2]);
    uint16_t Sz = Src.Size;
    NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
    NdVar ExtA = S.makeTemp(Sz * 2);
    NdVar ExtB = S.makeTemp(Sz * 2);
    S.emit(NdOp::INT_ZEXT, ExtA, {Rdx});
    S.emit(NdOp::INT_ZEXT, ExtB, {Src});
    NdVar Full = S.makeTemp(Sz * 2);
    S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
    S.emit(NdOp::SUBBYTES, DstLo, {Full, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {Full, NdVar::cst(Sz, 4)});
    break;
  }

  case X86_INS_PDEP: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Mask = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pdep, Dst, {Src, Mask});
    break;
  }
  case X86_INS_PEXT: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Mask = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pext, Dst, {Src, Mask});
    break;
  }

  case X86_INS_RORX: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar CntRaw = operandRead(S, X86.operands[2]);
    uint16_t Sz = Dst.Size;
    uint16_t Bits = Sz * 8;
    uint64_t RorxMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::cst(RorxMask, Sz)});
    NdVar Shr = S.makeTemp(Sz);
    NdVar Comp = S.makeTemp(Sz);
    NdVar Shl = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shr, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Sz), Cnt});
    S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::cst(Bits - 1, Sz)});
    S.emit(NdOp::INT_LEFT, Shl, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {Shr, Shl});
    break;
  }

  case X86_INS_SARX:
  case X86_INS_SHLX:
  case X86_INS_SHRX: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar CntRaw = operandRead(S, X86.operands[2]);
    uint16_t Sz = Dst.Size;
    uint16_t Bits = Sz * 8;
    uint64_t VexMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::cst(VexMask, Sz)});
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_SHLX:
      Opc = NdOp::INT_LEFT;
      break;
    case X86_INS_SHRX:
      Opc = NdOp::INT_RIGHT;
      break;
    default:
      Opc = NdOp::INT_ASHR;
    }
    S.emit(Opc, Dst, {Src, Cnt});
    break;
  }

  // ========================================================================
  // ADX: ADCX / ADOX — multi-precision addition (reads/writes single flag).
  // ========================================================================
  case X86_INS_ADCX: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar CfExt = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(x86reg::CF, 1)});
    NdVar C1 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C1, {Src, CfExt});
    NdVar Adj = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ADD, Adj, {Src, CfExt});
    // Carry-out reads the pre-write destination; compute it before the result
    // write so DstR does not alias-resolve to the post-add value.
    NdVar C2 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C2, {DstR, Adj});
    S.emit(NdOp::INT_ADD, DstW, {DstR, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {C1, C2});
    break;
  }

  case X86_INS_ADOX: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar OfExt = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ZEXT, OfExt, {NdVar::reg(x86reg::OF, 1)});
    NdVar C1 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C1, {Src, OfExt});
    NdVar Adj = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ADD, Adj, {Src, OfExt});
    // Carry-out reads the pre-write destination; compute it before the result
    // write so DstR does not alias-resolve to the post-add value.
    NdVar C2 = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, C2, {DstR, Adj});
    S.emit(NdOp::INT_ADD, DstW, {DstR, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::OF, 1), {C1, C2});
    break;
  }

  // --- SYSENTER / SYSEXIT / SYSRET ---
  case X86_INS_SYSENTER:
  case X86_INS_SYSEXIT:
  case X86_INS_SYSRET:
    S.emitIntrinsic(Intrinsic::Syscall);
    break;

  // --- RSM (return from SMM) ---
  case X86_INS_RSM: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    S.emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, PtrSize)});
    break;
  }

  // --- INT1 (ICEBP) ---
  case X86_INS_INT1:
    S.emitIntrinsic(Intrinsic::Int1);
    break;

  // --- CLI / STI ---
  case X86_INS_CLI:
  case X86_INS_STI:
    S.emitIntrinsic(InsnId == X86_INS_CLI ? Intrinsic::Cli : Intrinsic::Sti);
    break;

  // --- UD0 / UD1 ---
  case X86_INS_UD0:
  case X86_INS_UD1:
    S.emitIntrinsic(InsnId == X86_INS_UD0 ? Intrinsic::Ud0 : Intrinsic::Ud1);
    break;

  // --- INSD / OUTSD ---
  case X86_INS_INSD:
  case X86_INS_OUTSD:
    S.emitIntrinsic(InsnId == X86_INS_INSD ? Intrinsic::Insd
                                           : Intrinsic::Outsd);
    break;

  // ========================================================================
  // Port I/O — IN/OUT.  Capture the port (imm8 const, or DX register) and the
  // accumulator (AL/AX/EAX, size from the operand) so codegen can re-emit a
  // valid `in`/`out`.  A bare `in`/`out` is rejected (too few operands).
  // ========================================================================
  case X86_INS_IN: {
    // in acc, port   — read I/O port into the accumulator (value-producing).
    uint16_t Sz = (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG)
                      ? static_cast<uint16_t>(X86.operands[0].size)
                      : 4;
    if (Sz != 1 && Sz != 2 && Sz != 4)
      Sz = 4;
    NdVar Port =
        (X86.op_count >= 2 && X86.operands[1].type == X86_OP_IMM)
            ? NdVar::cst(static_cast<uint64_t>(X86.operands[1].imm) & 0xFF, 1)
            : NdVar::reg(x86reg::RDX, 2);
    S.emitIntrinsic(Intrinsic::In, NdVar::reg(x86reg::RAX, Sz), {Port});
    break;
  }
  case X86_INS_OUT: {
    // out port, acc  — write the accumulator to an I/O port (side-effect).
    uint16_t Sz = (X86.op_count >= 2 && X86.operands[1].type == X86_OP_REG)
                      ? static_cast<uint16_t>(X86.operands[1].size)
                      : 4;
    if (Sz != 1 && Sz != 2 && Sz != 4)
      Sz = 4;
    NdVar Port =
        (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM)
            ? NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm) & 0xFF, 1)
            : NdVar::reg(x86reg::RDX, 2);
    S.emitIntrinsic(Intrinsic::Out, NdVar::reg(x86reg::RAX, 8),
                    {Port, NdVar::reg(x86reg::RAX, Sz)});
    break;
  }

  // ========================================================================
  // Descriptor-table loads/stores + INVLPG — capture the memory-address
  // operand (best-effort: base register, as CLFLUSH does) so codegen can
  // re-emit `mnemonic (addr)`.  A bare mnemonic would be rejected by the
  // assembler (too few operands).
  // ========================================================================
  case X86_INS_LGDT:
  case X86_INS_LIDT:
  case X86_INS_SGDT:
  case X86_INS_SIDT:
  case X86_INS_INVLPG: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_LGDT:
      Id = Intrinsic::Lgdt;
      break;
    case X86_INS_LIDT:
      Id = Intrinsic::Lidt;
      break;
    case X86_INS_SGDT:
      Id = Intrinsic::Sgdt;
      break;
    case X86_INS_SIDT:
      Id = Intrinsic::Sidt;
      break;
    default:
      Id = Intrinsic::Invlpg;
      break;
    }
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM &&
        X86.operands[0].mem.base != X86_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].mem.base));
      S.emitIntrinsic(Id, NdVar::reg(x86reg::RAX, 8),
                      {NdVar::reg(RI.Offset, 8)});
    } else {
      S.emitIntrinsic(Id);
    }
    break;
  }

  // ========================================================================
  // r/m16 system-register loads/stores: LLDT/LTR/LMSW read r/m16,
  // SLDT/STR/SMSW write r/m16.  Capture the operand so codegen can re-emit a
  // valid mnemonic.  Discriminator: a memory base is captured as an 8-byte
  // pointer input; a register source is captured as a 2-byte value input; a
  // register destination (store form) becomes the INTRINSIC output.
  // ========================================================================
  case X86_INS_LLDT:
  case X86_INS_LTR:
  case X86_INS_LMSW:
  case X86_INS_SLDT:
  case X86_INS_STR:
  case X86_INS_SMSW: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_LLDT:
      Id = Intrinsic::Lldt;
      break;
    case X86_INS_LTR:
      Id = Intrinsic::Ltr;
      break;
    case X86_INS_LMSW:
      Id = Intrinsic::Lmsw;
      break;
    case X86_INS_SLDT:
      Id = Intrinsic::Sldt;
      break;
    case X86_INS_STR:
      Id = Intrinsic::Str;
      break;
    default:
      Id = Intrinsic::Smsw;
      break;
    }
    bool IsStore = (InsnId == X86_INS_SLDT || InsnId == X86_INS_STR ||
                    InsnId == X86_INS_SMSW);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM &&
        X86.operands[0].mem.base != X86_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].mem.base));
      S.emitIntrinsic(Id, NdVar::reg(x86reg::RAX, 8),
                      {NdVar::reg(RI.Offset, 8)});
    } else if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      if (IsStore)
        S.emitIntrinsic(Id, NdVar::reg(RI.Offset, 2), {});
      else
        S.emitIntrinsic(Id, NdVar::reg(x86reg::RAX, 8),
                        {NdVar::reg(RI.Offset, 2)});
    } else {
      S.emitIntrinsic(Id);
    }
    break;
  }

  // ========================================================================
  // Privileged / system instructions — I/O, MSRs, virtualization, etc.
  // ========================================================================
  case X86_INS_INSB:
  case X86_INS_INSW:
  case X86_INS_OUTSB:
  case X86_INS_OUTSW:
  case X86_INS_RDMSR:
  case X86_INS_WRMSR:
  case X86_INS_RDPMC:
  case X86_INS_RDPID:
  case X86_INS_RDRAND:
  case X86_INS_RDSEED:
  case X86_INS_RDFSBASE:
  case X86_INS_RDGSBASE:
  case X86_INS_WRFSBASE:
  case X86_INS_WRGSBASE:
  case X86_INS_VMCALL:
  case X86_INS_VMMCALL:
  case X86_INS_VMRUN:
  case X86_INS_VMSAVE:
  case X86_INS_VMLOAD:
  case X86_INS_VMLAUNCH:
  case X86_INS_VMRESUME:
  case X86_INS_VMXOFF:
  case X86_INS_VMXON:
  case X86_INS_VMCLEAR:
  case X86_INS_VMPTRLD:
  case X86_INS_VMPTRST:
  case X86_INS_VMREAD:
  case X86_INS_VMWRITE:
  case X86_INS_VMFUNC:
  case X86_INS_STGI:
  case X86_INS_CLGI:
  case X86_INS_SKINIT:
  case X86_INS_HLT:
  case X86_INS_INVD:
  case X86_INS_WBINVD:
  case X86_INS_INVPCID:
  case X86_INS_INVEPT:
  case X86_INS_INVVPID:
  case X86_INS_SWAPGS:
  case X86_INS_VERR:
  case X86_INS_VERW:
  case X86_INS_LAR:
  case X86_INS_LSL:
  case X86_INS_ARPL:
  case X86_INS_CLTS:
  case X86_INS_XSETBV:
  case X86_INS_XSAVE:
  case X86_INS_XSAVEC:
  case X86_INS_XSAVES:
  case X86_INS_XSAVEOPT:
  case X86_INS_XRSTOR:
  case X86_INS_XRSTORS:
  case X86_INS_MONITOR:
  case X86_INS_MWAIT:
  case X86_INS_MONITORX:
  case X86_INS_MWAITX:
  case X86_INS_GETSEC:
  case X86_INS_ENCLS:
  case X86_INS_ENCLU:
  case X86_INS_ENCLV:
  case X86_INS_TPAUSE:
  case X86_INS_UMONITOR:
  case X86_INS_UMWAIT: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_INSB:
      Id = Intrinsic::Insb;
      break;
    case X86_INS_INSW:
      Id = Intrinsic::Insw;
      break;
    case X86_INS_OUTSB:
      Id = Intrinsic::Outsb;
      break;
    case X86_INS_OUTSW:
      Id = Intrinsic::Outsw;
      break;
    case X86_INS_RDMSR:
      Id = Intrinsic::Rdmsr;
      break;
    case X86_INS_WRMSR:
      Id = Intrinsic::Wrmsr;
      break;
    case X86_INS_RDPMC:
      Id = Intrinsic::Rdpmc;
      break;
    case X86_INS_VMCALL:
      Id = Intrinsic::Vmcall;
      break;
    case X86_INS_VMMCALL:
      Id = Intrinsic::Vmmcall;
      break;
    case X86_INS_HLT:
      Id = Intrinsic::Hlt;
      break;
    case X86_INS_INVD:
      Id = Intrinsic::Invd;
      break;
    case X86_INS_WBINVD:
      Id = Intrinsic::Wbinvd;
      break;
    case X86_INS_SWAPGS:
      Id = Intrinsic::Swapgs;
      break;
    default:
      Id = Intrinsic::Hlt;
      break;
    }
    S.emitIntrinsic(Id);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
