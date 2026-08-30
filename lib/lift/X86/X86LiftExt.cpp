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
  auto LiftOuts = [&](Intrinsic Id, unsigned ElemSz) {
    const uint16_t AddrSz = S.AddressSize;
    bool Repeat = false;
    for (uint8_t Prefix : X86.prefix)
      Repeat |= Prefix == X86_PREFIX_REP || Prefix == X86_PREFIX_REPNE;
    NdVar Count =
        Repeat ? NdVar::reg(x86reg::RCX, AddrSz) : NdVar::scalar(1, AddrSz);
    NdVar Df = NdVar::reg(x86reg::DF, 1);
    S.emitIntrinsic(Id, NdVar(),
                    {NdVar::reg(x86reg::RSI, AddrSz), Count,
                     NdVar::reg(x86reg::RDX, 2), Df},
                    NdMemoryOrdering::None, stringSourceAddressSpace(X86));

    NdVar Bytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_MULT, Bytes, {Count, NdVar::scalar(ElemSz, AddrSz)});
    NdVar NegBytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, AddrSz), Bytes});
    NdVar Delta = S.makeTemp(AddrSz);
    S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
    NdVar NewSi = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, AddrSz), Delta});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, AddrSz), {NewSi});
    if (Repeat)
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, AddrSz),
             {NdVar::scalar(0, AddrSz)});
  };
  auto LiftIns = [&](Intrinsic Id, unsigned ElemSz) {
    const uint16_t AddrSz = S.AddressSize;
    bool Repeat = false;
    for (uint8_t Prefix : X86.prefix)
      Repeat |= Prefix == X86_PREFIX_REP || Prefix == X86_PREFIX_REPNE;
    NdVar Count =
        Repeat ? NdVar::reg(x86reg::RCX, AddrSz) : NdVar::scalar(1, AddrSz);
    NdVar Df = NdVar::reg(x86reg::DF, 1);
    S.emitIntrinsic(Id, NdVar(),
                    {NdVar::reg(x86reg::RDI, AddrSz), Count,
                     NdVar::reg(x86reg::RDX, 2), Df});

    NdVar Bytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_MULT, Bytes, {Count, NdVar::scalar(ElemSz, AddrSz)});
    NdVar NegBytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, AddrSz), Bytes});
    NdVar Delta = S.makeTemp(AddrSz);
    S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
    NdVar NewDi = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, AddrSz), Delta});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, AddrSz), {NewDi});
    if (Repeat)
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, AddrSz),
             {NdVar::scalar(0, AddrSz)});
  };
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
    LiftIns(Intrinsic::Insd, 4);
    break;
  case X86_INS_OUTSD:
    LiftOuts(Intrinsic::Outsd, 4);
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
    S.emitIntrinsic(Intrinsic::Out, NdVar(),
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
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(Id, X86.operands[0]))
      return false;
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
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      if (!S.emitMemoryIntrinsic(Id, X86.operands[0]))
        return false;
    } else if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      if (IsStore)
        S.emitIntrinsic(Id, NdVar::reg(RI.Offset, RI.Size), {});
      else
        S.emitIntrinsic(Id, NdVar(), {NdVar::reg(RI.Offset, 2)});
    } else {
      return false;
    }
    break;
  }

  // ========================================================================
  // Privileged / system instructions — I/O, MSRs, virtualization, etc.
  // ========================================================================
  case X86_INS_OUTSB:
    LiftOuts(Intrinsic::Outsb, 1);
    break;
  case X86_INS_OUTSW:
    LiftOuts(Intrinsic::Outsw, 2);
    break;
  case X86_INS_INSB:
    LiftIns(Intrinsic::Insb, 1);
    break;
  case X86_INS_INSW:
    LiftIns(Intrinsic::Insw, 2);
    break;
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

  case X86_INS_XSAVE:
  case X86_INS_XSAVEC:
  case X86_INS_XSAVES:
  case X86_INS_XSAVEOPT:
  case X86_INS_XRSTOR:
  case X86_INS_XRSTORS: {
    Intrinsic Id = Intrinsic::Xsave;
    switch (InsnId) {
    case X86_INS_XSAVEC:
      Id = Intrinsic::Xsavec;
      break;
    case X86_INS_XSAVES:
      Id = Intrinsic::Xsaves;
      break;
    case X86_INS_XSAVEOPT:
      Id = Intrinsic::Xsaveopt;
      break;
    case X86_INS_XRSTOR:
      Id = Intrinsic::Xrstor;
      break;
    case X86_INS_XRSTORS:
      Id = Intrinsic::Xrstors;
      break;
    default:
      break;
    }
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(
            Id, X86.operands[0],
            {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RDX, 4)}))
      return false;
    break;
  }

  default:
    return liftExtBMI(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
