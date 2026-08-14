//===- ARMLiftMemMultiple.cpp - ARM32 multi-register load/store lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PUSH and POP plus the load/store-multiple family LDM/LDMIB/LDMDA/
/// LDMDB and STM/STMIB/STMDA/STMDB.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMemMultiple(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  // --- PUSH / POP ---
  case ARM_INS_PUSH: {
    int NumRegs = ARM.op_count;
    NdVar Sp = NdVar::reg(armreg::SP, 4);
    S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
    for (int I = 0; I < NumRegs; I++) {
      NdVar R = L.operandRead(S, ARM.operands[I]);
      NdVar Slot = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
      S.emit(NdOp::STORE, {}, {Slot, R});
    }
    break;
  }
  case ARM_INS_POP: {
    int NumRegs = ARM.op_count;
    NdVar Sp = NdVar::reg(armreg::SP, 4);
    for (int I = 0; I < NumRegs; I++) {
      NdVar Slot = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
      NdVar Val = S.makeTemp(4);
      S.emit(NdOp::LOAD, Val, {Slot});
      NdVar Dst = L.operandWrite(ARM.operands[I]);
      S.emit(NdOp::COPY, Dst, {Val});
    }
    S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
    break;
  }

  // --- LDM / STM ---
  case ARM_INS_LDM:
  case ARM_INS_LDMIB:
  case ARM_INS_LDMDA:
  case ARM_INS_LDMDB: {
    // capstone decodes `pop {regs}` (= ldmia sp!, {regs}) as ARM_INS_LDM with
    // mnemonic "pop" and NO explicit base operand: every operand is part of the
    // register list and SP is the implicit base.  The generic path below treats
    // operands[0] as the base, which would use the first popped register as the
    // load address and drop one register from the list.
    if (isAliasMnemonic(Insn, "pop")) {
      int NumRegs = ARM.op_count;
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      for (int I = 0; I < NumRegs; I++) {
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
        NdVar Val = S.makeTemp(4);
        S.emit(NdOp::LOAD, Val, {Slot});
        NdVar Dst = L.operandWrite(ARM.operands[I]);
        S.emit(NdOp::COPY, Dst, {Val});
      }
      S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
      break;
    }
    if (ARM.op_count < 2)
      break;
    auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
    NdVar Base = NdVar::reg(RI.Offset, 4);
    NdVar BaseAddr = S.makeTemp(4);
    S.emit(NdOp::COPY, BaseAddr, {Base});

    int OffsetStart = 0;
    int Delta = 4;
    if (Insn->id == ARM_INS_LDMIB) {
      OffsetStart = 4;
    } else if (Insn->id == ARM_INS_LDMDA) {
      OffsetStart = -4 * (ARM.op_count - 2);
    } else if (Insn->id == ARM_INS_LDMDB) {
      OffsetStart = -4 * (ARM.op_count - 1);
    }

    std::vector<std::pair<NdVar, NdVar>> LoadedRegs;
    LoadedRegs.reserve(ARM.op_count - 1);
    for (int I = 1; I < ARM.op_count; I++) {
      int Off = OffsetStart + (I - 1) * Delta;
      NdVar EA = S.makeTemp(4);
      S.emit(
          NdOp::INT_ADD, EA,
          {BaseAddr,
           NdVar::cst(static_cast<uint64_t>(static_cast<uint32_t>(Off)), 4)});
      NdVar Val = S.makeTemp(4);
      S.emit(NdOp::LOAD, Val, {EA});
      NdVar Dst = L.operandWrite(ARM.operands[I]);
      LoadedRegs.emplace_back(Dst, Val);
    }
    // LDM reads every word using the pre-instruction base value.  Delay all
    // architectural register writes until the complete register list has
    // been loaded: the base itself may legally appear in a non-writeback list
    // (for example `ldm r1, {r1, r7}`).
    for (const auto &[Dst, Val] : LoadedRegs)
      S.emit(NdOp::COPY, Dst, {Val});
    if (Insn->detail->writeback) {
      int Total = (ARM.op_count - 1) * 4;
      if (Insn->id == ARM_INS_LDM || Insn->id == ARM_INS_LDMIB)
        S.emit(NdOp::INT_ADD, Base, {BaseAddr, NdVar::cst(Total, 4)});
      else
        S.emit(NdOp::INT_SUB, Base, {BaseAddr, NdVar::cst(Total, 4)});
    }
    break;
  }
  case ARM_INS_STM:
  case ARM_INS_STMIB:
  case ARM_INS_STMDA:
  case ARM_INS_STMDB: {
    // capstone decodes `push {regs}` (= stmdb sp!, {regs}) as ARM_INS_STMDB
    // with mnemonic "push" and NO explicit base operand: every operand is part
    // of the register list and SP is the implicit base.  Treating operands[0]
    // as the base (the generic path below) would use the first pushed register
    // as the store address, drop one register, and — critically — leave SP
    // undecremented so the callee-saved save area is missing from the computed
    // frame size.
    if (isAliasMnemonic(Insn, "push")) {
      int NumRegs = ARM.op_count;
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
      for (int I = 0; I < NumRegs; I++) {
        NdVar R = L.operandRead(S, ARM.operands[I]);
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
        S.emit(NdOp::STORE, {}, {Slot, R});
      }
      break;
    }
    if (ARM.op_count < 2)
      break;
    auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
    NdVar Base = NdVar::reg(RI.Offset, 4);

    int OffsetStart = 0;
    int Delta = 4;
    if (Insn->id == ARM_INS_STMIB) {
      OffsetStart = 4;
    } else if (Insn->id == ARM_INS_STMDA) {
      OffsetStart = -4 * (ARM.op_count - 2);
    } else if (Insn->id == ARM_INS_STMDB) {
      OffsetStart = -4 * (ARM.op_count - 1);
    }

    for (int I = 1; I < ARM.op_count; I++) {
      NdVar Src = L.operandRead(S, ARM.operands[I]);
      int Off = OffsetStart + (I - 1) * Delta;
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Off)), 4)});
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    if (Insn->detail->writeback) {
      int Total = (ARM.op_count - 1) * 4;
      if (Insn->id == ARM_INS_STM || Insn->id == ARM_INS_STMIB)
        S.emit(NdOp::INT_ADD, Base, {Base, NdVar::cst(Total, 4)});
      else
        S.emit(NdOp::INT_SUB, Base, {Base, NdVar::cst(Total, 4)});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
