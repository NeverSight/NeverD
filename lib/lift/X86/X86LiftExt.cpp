//===- X86LiftExt.cpp - x86/x64 extension instruction lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the x86/x64 extension instructions.  The BMI1/BMI2/ADX
/// bit-manipulation handlers are in X86LiftExtBMI.cpp; the privileged,
/// segment/descriptor, port-I/O and virtualization instructions stay here
/// because RSM reads the lifter's private target architecture.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftExt(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

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
  // r/m16 system-register loads/stores: LLDT/LTR/LMSW read r/m16, while the
  // memory forms of SLDT/STR/SMSW write 16 bits.  Their register-destination
  // forms retain the decoded register width so r32/r64 receive the selector or
  // machine-status word zero-extended to the full destination.  A memory base
  // is captured as an 8-byte pointer input; a register destination becomes the
  // INTRINSIC output.
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
        S.emitIntrinsic(Id, NdVar::reg(RI.Offset, RI.Size), {});
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
    return liftExtBMI(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
