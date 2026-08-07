//===- X86LiftControl.cpp - x86/x64 control-flow instruction lifter ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow instruction handlers for x86/x64: PUSH/POP, CALL/RET,
/// Jcc/JMP, SETcc, CMOVcc, LEAVE, and LOOP variants.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftControl(LiftState &S, const cs_insn *Insn,
                            const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- PUSH ---
  case X86_INS_PUSH: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t PushSz = Src.Size > 0 ? Src.Size : PtrSize;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PushSz, PtrSize)});
    S.emit(NdOp::STORE, {}, {Rsp, Src});
    break;
  }

  // --- POP ---
  case X86_INS_POP: {
    if (X86.op_count < 1)
      break;
    NdVar DstW = operandWrite(X86.operands[0]);
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t PopSz = DstW.Size > 0 ? DstW.Size : PtrSize;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    NdVar Val = S.makeTemp(PopSz);
    S.emit(NdOp::LOAD, Val, {Rsp});
    S.emit(NdOp::COPY, DstW, {Val});
    // get-PC thunk: a `pop` immediately after `call $+5` yields the constant PC
    // (the i386 PIC GOT-base seed), not a stack value.  Overwrite the loaded
    // value with the constant so the GOT/constant-pool address folds.
    if (GetPcArmedThisInsn)
      S.emit(NdOp::COPY, DstW, {NdVar::cst(GetPcValue, PopSz)});
    S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::cst(PopSz, PtrSize)});
    break;
  }

  // --- CALL ---
  case X86_INS_CALL: {
    if (X86.op_count < 1)
      break;
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    if (X86.operands[0].type == X86_OP_IMM) {
      uint64_t Target = static_cast<uint64_t>(X86.operands[0].imm);
      // get-PC thunk: `call $+5` (target == the next instruction) only pushes
      // the return address so a following `pop reg` loads the current PC — the
      // i386 PIC idiom for addressing the constant pool / globals (32-bit x86
      // has no EIP-relative addressing).  Model it as a plain push of the
      // next-instruction address rather than a real call, so the popped value
      // is a known constant VA the emitter resolves to rodata.
      if (Target == S.Addr + S.InsnSize) {
        NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
        S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
        S.emit(NdOp::STORE, {}, {Rsp, NdVar::cst(Target, PtrSize)});
        GetPcPending = true;
        GetPcValue = Target;
      } else {
        S.emit(NdOp::CALL, NdVar::reg(x86reg::RAX, PtrSize),
               {NdVar::cst(Target, PtrSize)});
      }
    } else if (X86.operands[0].type == X86_OP_MEM &&
               X86.operands[0].mem.base == X86_REG_RIP &&
               X86.operands[0].mem.index == X86_REG_INVALID) {
      // RIP-relative indirect call: call [rip + disp].
      // Compute the absolute address of the IAT/GOT slot. Using a
      // constant input lets MedABIPass resolve the import name.
      uint64_t SlotAddr =
          S.Addr + S.InsnSize + static_cast<uint64_t>(X86.operands[0].mem.disp);
      S.emit(NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, PtrSize),
             {NdVar::cst(SlotAddr, PtrSize)});
    } else {
      NdVar Target = operandRead(S, X86.operands[0]);
      S.emit(NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, PtrSize), {Target});
    }
    break;
  }

  // --- RET ---
  case X86_INS_RET: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    // `ret imm` is the i386 SysV callee-cleanup form (the callee pops imm extra
    // bytes off the caller stack — used for the hidden struct-return (sret)
    // pointer).  Record the function's largest pop so a caller adds it to its
    // post-call stack pointer; an ordinary `ret` leaves it 0.
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM) {
      int Pop = static_cast<int>(X86.operands[0].imm);
      if (Pop > FuncRetPopBytes)
        FuncRetPopBytes = Pop;
    }
    S.emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, PtrSize)});
    break;
  }

  // --- Jcc (conditional jumps) ---
  case X86_INS_JE:
  case X86_INS_JNE:
  case X86_INS_JA:
  case X86_INS_JAE:
  case X86_INS_JB:
  case X86_INS_JBE:
  case X86_INS_JG:
  case X86_INS_JGE:
  case X86_INS_JL:
  case X86_INS_JLE:
  case X86_INS_JS:
  case X86_INS_JNS:
  case X86_INS_JO:
  case X86_INS_JNO:
  case X86_INS_JP:
  case X86_INS_JNP:
  case X86_INS_JCXZ:
  case X86_INS_JECXZ:
  case X86_INS_JRCXZ: {
    if (X86.op_count < 1)
      break;
    va_t Target = static_cast<uint64_t>(X86.operands[0].imm);
    NdVar Cond = S.makeTemp(1);

    switch (InsnId) {
    case X86_INS_JE:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_JNE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_JB:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_JAE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_JA: {
      NdVar NC = S.makeTemp(1);
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NC, NZ});
      break;
    }
    case X86_INS_JBE: {
      S.emit(NdOp::BOOL_OR, Cond,
             {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
      break;
    }
    case X86_INS_JG: {
      NdVar NZ = S.makeTemp(1);
      NdVar SfEqOf = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::INT_EQUAL, SfEqOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, SfEqOf});
      break;
    }
    case X86_INS_JGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JL:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JLE: {
      NdVar SfNeOf = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SfNeOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(x86reg::ZF, 1), SfNeOf});
      break;
    }
    case X86_INS_JS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_JNS:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_JO:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JNO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JP:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_JNP:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_JCXZ:
    case X86_INS_JECXZ:
    case X86_INS_JRCXZ: {
      uint16_t CxSize = (InsnId == X86_INS_JCXZ)    ? 2
                        : (InsnId == X86_INS_JECXZ) ? 4
                                                    : 8;
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::RCX, CxSize), NdVar::cst(0, CxSize)});
      break;
    }
    default:
      break;
    }
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(Target, 8), Cond});
    break;
  }

  // --- JMP ---
  case X86_INS_JMP: {
    if (X86.op_count < 1)
      break;
    if (X86.operands[0].type == X86_OP_IMM) {
      S.emit(NdOp::BRANCH, {},
             {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 8)});
    } else if (X86.operands[0].type == X86_OP_MEM &&
               X86.operands[0].mem.base == X86_REG_RIP &&
               X86.operands[0].mem.index == X86_REG_INVALID) {
      uint64_t SlotAddr =
          S.Addr + S.InsnSize + static_cast<uint64_t>(X86.operands[0].mem.disp);
      S.emit(NdOp::INDIR_BR, {}, {NdVar::cst(SlotAddr, 8)});
    } else {
      NdVar Target = operandRead(S, X86.operands[0]);
      S.emit(NdOp::INDIR_BR, {}, {Target});
    }
    break;
  }

  // --- SETCC ---
  case X86_INS_SETE:
  case X86_INS_SETNE:
  case X86_INS_SETA:
  case X86_INS_SETAE:
  case X86_INS_SETB:
  case X86_INS_SETBE:
  case X86_INS_SETG:
  case X86_INS_SETGE:
  case X86_INS_SETL:
  case X86_INS_SETLE:
  case X86_INS_SETS:
  case X86_INS_SETNS:
  case X86_INS_SETO:
  case X86_INS_SETNO:
  case X86_INS_SETP:
  case X86_INS_SETNP: {
    if (X86.op_count < 1)
      break;
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Cond = S.makeTemp(1);

    switch (InsnId) {
    case X86_INS_SETE:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_SETNE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_SETB:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_SETAE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_SETS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_SETNS:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_SETO:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETNO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETP:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_SETNP:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_SETA: {
      NdVar NC = S.makeTemp(1);
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NC, NZ});
      break;
    }
    case X86_INS_SETBE:
      S.emit(NdOp::BOOL_OR, Cond,
             {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_SETG: {
      NdVar NZ = S.makeTemp(1);
      NdVar SfEqOf = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::INT_EQUAL, SfEqOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, SfEqOf});
      break;
    }
    case X86_INS_SETGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETL:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETLE: {
      NdVar SfNeOf = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SfNeOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(x86reg::ZF, 1), SfNeOf});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(0, 1)});
      break;
    }

    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Cond);
    } else {
      // SETcc writes exactly ONE byte to the destination register's low byte
      // (AL/BL/.../R15B) or high byte (AH/BH/CH/DH); the enclosing 32/64-bit
      // register's OTHER bytes are PRESERVED.  Writing the byte through a
      // 32-bit INT_ZEXT (the old approach) zeroes bits 63:8 of the 64-bit
      // register (a 32-bit write zero-extends) — wrong for any caller that
      // keeps live data in the upper bytes.  A bare 1-byte register write is
      // correct on paper but lands the flag-derived value in a byte
      // sub-register that the downstream sub-register reconstruction mishandles
      // when it trails a wider write (`xor eax,eax; setcc al` — the compiler's
      // standard idiom — would drop the byte).  Instead read-modify-write the
      // FULL register: zero- extend the condition to register width (Pass 2
      // still recovers the comparison feeding this INT_ZEXT, as for the old
      // wide-ZEXT form), mask the target byte out of the old value, OR the
      // condition in, and store the whole register back.  Wide read + wide
      // write only — no trailing partial write — so upper bytes are preserved
      // without tripping the reconstruction.
      uint16_t WideSz = (TargetArch == Arch::X64) ? 8 : 4;
      uint64_t ByteOff = DstW.Offset % 8; // 0 for *L / 1 for AH/BH/CH/DH
      uint64_t EnclOff = DstW.Offset - ByteOff;
      NdVar Wide = NdVar::reg(EnclOff, WideSz);

      NdVar CondW = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ZEXT, CondW, {Cond});
      if (ByteOff > 0) {
        NdVar Shifted = S.makeTemp(WideSz);
        S.emit(NdOp::INT_LEFT, Shifted,
               {CondW, NdVar::cst(ByteOff * 8, WideSz)});
        CondW = Shifted;
      }
      uint64_t WideMask = (WideSz >= 8) ? ~0ULL : ((1ULL << (WideSz * 8)) - 1);
      uint64_t ClearMask = (~(0xFFULL << (ByteOff * 8))) & WideMask;
      NdVar Cleared = S.makeTemp(WideSz);
      S.emit(NdOp::INT_AND, Cleared, {Wide, NdVar::cst(ClearMask, WideSz)});
      NdVar Result = S.makeTemp(WideSz);
      S.emit(NdOp::INT_OR, Result, {Cleared, CondW});
      S.emit(NdOp::COPY, Wide, {Result});
    }
    break;
  }

  // --- LEAVE ---
  case X86_INS_LEAVE: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    NdVar Rbp = NdVar::reg(x86reg::RBP, PtrSize);
    S.emit(NdOp::COPY, Rsp, {Rbp});
    NdVar Val = S.makeTemp(PtrSize);
    S.emit(NdOp::LOAD, Val, {Rsp});
    S.emit(NdOp::COPY, Rbp, {Val});
    S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
    break;
  }

  // --- CMOV ---
  case X86_INS_CMOVE:
  case X86_INS_CMOVNE:
  case X86_INS_CMOVA:
  case X86_INS_CMOVAE:
  case X86_INS_CMOVB:
  case X86_INS_CMOVBE:
  case X86_INS_CMOVG:
  case X86_INS_CMOVGE:
  case X86_INS_CMOVL:
  case X86_INS_CMOVLE:
  case X86_INS_CMOVS:
  case X86_INS_CMOVNS:
  case X86_INS_CMOVO:
  case X86_INS_CMOVNO:
  case X86_INS_CMOVP:
  case X86_INS_CMOVNP: {
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Dst = operandWrite(X86.operands[0]);
    uint16_t Sz = Dst.Size > 0 ? Dst.Size : DstR.Size;

    NdVar Cond = S.makeTemp(1);
    switch (InsnId) {
    case X86_INS_CMOVE:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_CMOVNE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_CMOVB:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_CMOVAE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_CMOVS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_CMOVNS:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_CMOVO:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVNO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVP:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_CMOVNP:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_CMOVA: {
      NdVar NC = S.makeTemp(1);
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NC, NZ});
      break;
    }
    case X86_INS_CMOVBE:
      S.emit(NdOp::BOOL_OR, Cond,
             {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_CMOVG: {
      NdVar NZ = S.makeTemp(1);
      NdVar SfEqOf = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::INT_EQUAL, SfEqOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, SfEqOf});
      break;
    }
    case X86_INS_CMOVGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVL:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVLE: {
      NdVar SfNeOf = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SfNeOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(x86reg::ZF, 1), SfNeOf});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }
    NdVar CondExt = S.makeTemp(Sz);
    S.emit(NdOp::INT_ZEXT, CondExt, {Cond});
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_NEG2, Mask, {CondExt});
    NdVar InvMask = S.makeTemp(Sz);
    S.emit(NdOp::INT_NOT, InvMask, {Mask});
    NdVar T1 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, T1, {Src, Mask});
    NdVar T2 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, T2, {DstR, InvMask});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }

  // --- LOOP / LOOPE / LOOPNE ---
  case X86_INS_LOOP:
  case X86_INS_LOOPE:
  case X86_INS_LOOPNE: {
    if (X86.op_count < 1)
      break;
    uint16_t CxSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rcx = NdVar::reg(x86reg::RCX, CxSize);
    S.emit(NdOp::INT_SUB, Rcx, {Rcx, NdVar::cst(1, CxSize)});
    NdVar NZ = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, NZ, {Rcx, NdVar::cst(0, CxSize)});
    NdVar Cond = NZ;
    if (InsnId == X86_INS_LOOPE) {
      NdVar Merged = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, Merged, {NZ, NdVar::reg(x86reg::ZF, 1)});
      Cond = Merged;
    } else if (InsnId == X86_INS_LOOPNE) {
      NdVar NZF = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZF, {NdVar::reg(x86reg::ZF, 1)});
      NdVar Merged = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, Merged, {NZ, NZF});
      Cond = Merged;
    }
    S.emit(NdOp::COND_BR, {},
           {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 8), Cond});
    break;
  }

  // --- PUSHA / PUSHAD ---
  case X86_INS_PUSHAW:
  case X86_INS_PUSHAL: {
    uint16_t PtSz = (InsnId == X86_INS_PUSHAL) ? 4 : 2;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtSz);
    for (uint64_t Reg : {x86reg::RAX, x86reg::RCX, x86reg::RDX, x86reg::RBX,
                         x86reg::RSP, x86reg::RBP, x86reg::RSI, x86reg::RDI}) {
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PtSz, PtSz)});
      S.emit(NdOp::STORE, {}, {Rsp, NdVar::reg(Reg, PtSz)});
    }
    break;
  }

  // --- POPA / POPAD ---
  case X86_INS_POPAW:
  case X86_INS_POPAL: {
    uint16_t PtSz = (InsnId == X86_INS_POPAL) ? 4 : 2;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtSz);
    for (uint64_t Reg : {x86reg::RDI, x86reg::RSI, x86reg::RBP, x86reg::RSP,
                         x86reg::RBX, x86reg::RDX, x86reg::RCX, x86reg::RAX}) {
      NdVar Val = S.makeTemp(PtSz);
      S.emit(NdOp::LOAD, Val, {Rsp});
      if (Reg != x86reg::RSP)
        S.emit(NdOp::COPY, NdVar::reg(Reg, PtSz), {Val});
      S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::cst(PtSz, PtSz)});
    }
    break;
  }

  // --- LCALL (far call) ---
  case X86_INS_LCALL: {
    if (X86.op_count < 1)
      break;
    NdVar Target = operandRead(S, X86.operands[0]);
    S.emit(NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, 4), {Target});
    break;
  }

  // --- LJMP (far jump) ---
  case X86_INS_LJMP: {
    if (X86.op_count < 1)
      break;
    if (X86.operands[0].type == X86_OP_IMM) {
      S.emit(NdOp::BRANCH, {},
             {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 8)});
    } else {
      NdVar Target = operandRead(S, X86.operands[0]);
      S.emit(NdOp::INDIR_BR, {}, {Target});
    }
    break;
  }

  // --- ENTER imm16, imm8 ---
  // imm8 is the lexical nesting level (taken mod 32).  Intel SDM:
  //   Push(RBP); FrameTemp = RSP
  //   IF level > 0:
  //     FOR i = 1 TO level-1:  RBP -= PtrSize; Push([RBP])   // copy display
  //     Push(FrameTemp)
  //   RBP = FrameTemp;  RSP -= imm16
  // The nesting level was previously ignored, which dropped the Push(FrameTemp)
  // (and, for level >= 2, the display copies) for nested frames.
  case X86_INS_ENTER: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    NdVar Rbp = NdVar::reg(x86reg::RBP, PtrSize);
    uint64_t AllocSz =
        (X86.op_count >= 1) ? static_cast<uint64_t>(X86.operands[0].imm) : 0;
    uint64_t Level = (X86.op_count >= 2)
                         ? (static_cast<uint64_t>(X86.operands[1].imm) % 32)
                         : 0;
    // Push(RBP)
    S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
    S.emit(NdOp::STORE, {}, {Rsp, Rbp});
    // FrameTemp = RSP
    NdVar FrameTemp = S.makeTemp(PtrSize);
    S.emit(NdOp::COPY, FrameTemp, {Rsp});
    if (Level > 0) {
      // Display copy: FOR i = 1 TO level-1: RBP -= PtrSize; Push([RBP]).
      for (uint64_t I = 1; I < Level; ++I) {
        S.emit(NdOp::INT_SUB, Rbp, {Rbp, NdVar::cst(PtrSize, PtrSize)});
        NdVar Disp = S.makeTemp(PtrSize);
        S.emit(NdOp::LOAD, Disp, {Rbp});
        S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
        S.emit(NdOp::STORE, {}, {Rsp, Disp});
      }
      // Push(FrameTemp)
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
      S.emit(NdOp::STORE, {}, {Rsp, FrameTemp});
    }
    // RBP = FrameTemp
    S.emit(NdOp::COPY, Rbp, {FrameTemp});
    // RSP -= imm16
    if (AllocSz)
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(AllocSz, PtrSize)});
    break;
  }

  // --- RETF / IRET ---
  case X86_INS_RETF:
  case X86_INS_RETFQ:
  case X86_INS_IRET:
  case X86_INS_IRETD:
  case X86_INS_IRETQ: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    S.emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, PtrSize)});
    break;
  }

  // --- PUSHF / POPF ---
  case X86_INS_PUSHF:
  case X86_INS_PUSHFD:
  case X86_INS_PUSHFQ:
  case X86_INS_POPF:
  case X86_INS_POPFD:
  case X86_INS_POPFQ: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    bool Push = (InsnId == X86_INS_PUSHF || InsnId == X86_INS_PUSHFD ||
                 InsnId == X86_INS_PUSHFQ);
    // Modelled EFLAGS bits at their architectural positions.  System flags
    // (TF/IF/IOPL/...) are not modelled; the previous code pushed/popped an
    // uninitialised temp, so flags were never actually (de)serialised.
    const std::pair<uint64_t, unsigned> FlagBits[] = {
        {x86reg::CF, 0}, {x86reg::PF, 2},  {x86reg::AF, 4}, {x86reg::ZF, 6},
        {x86reg::SF, 7}, {x86reg::DF, 10}, {x86reg::OF, 11}};
    if (Push) {
      // Assemble EFLAGS from the modelled flags (reserved bit 1 reads as 1).
      NdVar Eflags = S.makeTemp(PtrSize);
      S.emit(NdOp::COPY, Eflags, {NdVar::cst(0x2, PtrSize)});
      for (auto [Fl, Bit] : FlagBits) {
        NdVar Z = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_ZEXT, Z, {NdVar::reg(Fl, 1)});
        NdVar Sh = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_LEFT, Sh, {Z, NdVar::cst(Bit, PtrSize)});
        NdVar Next = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_OR, Next, {Eflags, Sh});
        Eflags = Next;
      }
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
      S.emit(NdOp::STORE, {}, {Rsp, Eflags});
    } else {
      // Load EFLAGS and scatter the modelled bits back into the flag registers.
      NdVar Val = S.makeTemp(PtrSize);
      S.emit(NdOp::LOAD, Val, {Rsp});
      S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::cst(PtrSize, PtrSize)});
      for (auto [Fl, Bit] : FlagBits) {
        NdVar Sh = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_RIGHT, Sh, {Val, NdVar::cst(Bit, PtrSize)});
        NdVar Bitv = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_AND, Bitv, {Sh, NdVar::cst(1, PtrSize)});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(Fl, 1),
               {Bitv, NdVar::cst(0, PtrSize)});
      }
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
