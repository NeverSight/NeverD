//===- X86LiftSystem.cpp - x86/x64 trap, serializing and system-call lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instructions that are not string operations but share the
/// string dispatcher: the INT3/UD2 traps, CPUID/XGETBV and the
/// timestamp counters, PAUSE, the LFENCE/SFENCE/MFENCE barriers,
/// the CLFLUSH/PREFETCH cache hints, and the SYSCALL/INT system
/// call entries.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSystem(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- Privileged / synchronization / debugger-trap instructions ---
  case X86_INS_INT3:
  case X86_INS_UD2:
  case X86_INS_CPUID:
  case X86_INS_XGETBV:
  case X86_INS_RDTSC:
  case X86_INS_RDTSCP:
  case X86_INS_PAUSE:
  case X86_INS_MFENCE:
  case X86_INS_LFENCE:
  case X86_INS_SFENCE:
  case X86_INS_CLFLUSH:
  case X86_INS_PREFETCH:
  case X86_INS_PREFETCHT0:
  case X86_INS_PREFETCHT1:
  case X86_INS_PREFETCHT2:
  case X86_INS_PREFETCHNTA: {
    Intrinsic Id = Intrinsic::None;
    std::vector<std::pair<uint64_t, uint16_t>> Writes;
    std::vector<NdVar> ExtraInputs;
    switch (InsnId) {
    case X86_INS_INT3:
      Id = Intrinsic::Int3;
      break;
    case X86_INS_UD2:
      Id = Intrinsic::Ud2;
      break;
    case X86_INS_CPUID:
      Id = Intrinsic::Cpuid;
      Writes = {{x86reg::RAX, 4},
                {x86reg::RBX, 4},
                {x86reg::RCX, 4},
                {x86reg::RDX, 4}};
      ExtraInputs = {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RCX, 4)};
      break;
    case X86_INS_RDTSC:
      Id = Intrinsic::Rdtsc;
      Writes = {{x86reg::RAX, 4}, {x86reg::RDX, 4}};
      break;
    case X86_INS_XGETBV:
      Id = Intrinsic::Xgetbv;
      Writes = {{x86reg::RAX, 4}, {x86reg::RDX, 4}};
      ExtraInputs = {NdVar::reg(x86reg::RCX, 4)};
      break;
    case X86_INS_PAUSE:
      Id = Intrinsic::Pause;
      break;
    case X86_INS_MFENCE:
      Id = Intrinsic::Mfence;
      break;
    case X86_INS_LFENCE:
      Id = Intrinsic::Lfence;
      break;
    case X86_INS_SFENCE:
      Id = Intrinsic::Sfence;
      break;
    case X86_INS_RDTSCP:
      Id = Intrinsic::Rdtscp;
      Writes = {{x86reg::RAX, 4}, {x86reg::RDX, 4}, {x86reg::RCX, 4}};
      break;
    case X86_INS_CLFLUSH:
      Id = Intrinsic::Clflush;
      if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM &&
          X86.operands[0].mem.base != X86_REG_INVALID) {
        auto RI =
            mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].mem.base));
        ExtraInputs = {NdVar::reg(RI.Offset, 8)};
      }
      break;
    default:
      Id = Intrinsic::Prefetch;
      break;
    }
    {
      LowOp LOp;
      LOp.Opcode = NdOp::INTRINSIC;
      LOp.Addr = S.Addr;
      LOp.Seq = S.Seq++;
      // A value-producing intrinsic (CPUID/RDTSC/XGETBV) carries its primary
      // result in RAX; a side-effect-only one (a fence / prefetch / trap, with
      // no Writes) carries NO output, so SSA never bumps RAX for it.  A bare
      // RAX destination on a void fence would shadow a live RAX (mirrors the
      // ARM `dmb` self-loop bug) with the op's never-assigned, zero-defaulted
      // output.
      LOp.Output = Writes.empty() ? NdVar() : NdVar::reg(x86reg::RAX, 8);
      LOp.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
      for (auto &V : ExtraInputs)
        LOp.addInput(V);
      S.Ops.push_back(LOp);
    }
    for (auto &[RegOff, Sz] : Writes) {
      NdVar Tmp = S.makeTemp(Sz);
      S.emit(NdOp::COPY, NdVar::reg(RegOff, Sz), {Tmp});
    }
    break;
  }

  // --- SYSCALL / INT ---
  case X86_INS_SYSCALL:
  case X86_INS_INT: {
    Intrinsic Id =
        (InsnId == X86_INS_SYSCALL) ? Intrinsic::Syscall : Intrinsic::IntN;
    if (InsnId == X86_INS_INT && X86.op_count >= 1 &&
        X86.operands[0].type == X86_OP_IMM && X86.operands[0].imm == 3) {
      Id = Intrinsic::Int3;
    }
    if (InsnId == X86_INS_INT && X86.op_count >= 1 &&
        X86.operands[0].type == X86_OP_IMM) {
      S.emitIntrinsic(
          Id, NdVar::reg(x86reg::RAX, 8),
          {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 1)});
    } else {
      S.emitIntrinsic(Id);
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
