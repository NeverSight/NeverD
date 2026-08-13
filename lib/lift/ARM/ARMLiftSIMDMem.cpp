//===- ARMLiftSIMDMem.cpp - ARM32 VFP load/store lifter ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// VLDR/VSTR, the VLDM/VSTM register-list transfers (including the
/// vpush/vpop aliases) and the lazy-state VLLDM/VLSTM/VSCCLRM.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDMem(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM) {
  switch (Insn->id) {
  // VFP load/store — use operandEffAddr to avoid double-LOAD
  case ARM_INS_VLDR:
  case ARM_INS_VLDRB:
  case ARM_INS_VLDRH:
  case ARM_INS_VLDRW:
  case ARM_INS_VLDRD: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[1])
                     : L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::LOAD, Dst, {EA});
    break;
  }
  case ARM_INS_VSTR:
  case ARM_INS_VSTRB:
  case ARM_INS_VSTRH:
  case ARM_INS_VSTRW:
  case ARM_INS_VSTRD: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[1])
                     : L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case ARM_INS_VLDMIA:
  case ARM_INS_VLDMDB:
  case ARM_INS_FLDMIAX:
  case ARM_INS_FLDMDBX: {
    // capstone decodes `vpop {regs}` (= vldmia sp!, {regs}) as VLDMIA with
    // mnemonic "vpop" and NO explicit base operand: SP is the implicit base and
    // every operand is a list register.  The generic path below would treat the
    // first restored register as the base, corrupting it.
    if (llvm::StringRef(Insn->mnemonic) == "vpop") {
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      unsigned Off = 0;
      for (int I = 0; I < ARM.op_count; I++) {
        NdVar Dst = L.operandWrite(ARM.operands[I]);
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(Off, 4)});
        S.emit(NdOp::LOAD, Dst, {Slot});
        Off += Dst.Size;
      }
      S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(Off, 4)});
      break;
    }
    // VLDM loads a LIST of VFP/NEON registers (S=4B / D=8B) from consecutive
    // memory.  The old handler loaded only operands[1], dropping the rest of
    // the list and the per-register address increment (e.g. `vldmia sp,
    // {d16,d17}` left d17 undefined — garbage in dot-product / VFP spill-reload
    // sequences). DB (decrement-before) starts at base - totalBytes.
    if (ARM.op_count < 2)
      break;
    NdVar Base = L.operandRead(S, ARM.operands[0]);
    bool IsDB = (Insn->id == ARM_INS_VLDMDB || Insn->id == ARM_INS_FLDMDBX);
    unsigned Total = 0;
    for (int I = 1; I < ARM.op_count; I++)
      Total += L.operandWrite(ARM.operands[I]).Size;
    int Cur = IsDB ? -static_cast<int>(Total) : 0;
    for (int I = 1; I < ARM.op_count; I++) {
      NdVar Dst = L.operandWrite(ARM.operands[I]);
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Cur)), 4)});
      S.emit(NdOp::LOAD, Dst, {EA});
      Cur += static_cast<int>(Dst.Size);
    }
    if (Insn->detail->writeback && ARM.operands[0].type == ARM_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
      NdVar BaseReg = NdVar::reg(RI.Offset, 4);
      S.emit(IsDB ? NdOp::INT_SUB : NdOp::INT_ADD, BaseReg,
             {BaseReg, NdVar::cst(Total, 4)});
    }
    break;
  }
  case ARM_INS_VSTMIA:
  case ARM_INS_VSTMDB:
  case ARM_INS_FSTMIAX:
  case ARM_INS_FSTMDBX: {
    // capstone decodes `vpush {regs}` (= vstmdb sp!, {regs}) as VSTMDB with
    // mnemonic "vpush" and NO explicit base operand: SP is the implicit base
    // and every operand is a list register (saved low-to-high at the
    // decremented SP).
    if (llvm::StringRef(Insn->mnemonic) == "vpush") {
      unsigned Total = 0;
      for (int I = 0; I < ARM.op_count; I++)
        Total += L.operandWrite(ARM.operands[I]).Size;
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(Total, 4)});
      unsigned Off = 0;
      for (int I = 0; I < ARM.op_count; I++) {
        NdVar R = L.operandRead(S, ARM.operands[I]);
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(Off, 4)});
        S.emit(NdOp::STORE, {}, {Slot, R});
        Off += R.Size;
      }
      break;
    }
    if (ARM.op_count < 2)
      break;
    NdVar Base = L.operandRead(S, ARM.operands[0]);
    bool IsDB = (Insn->id == ARM_INS_VSTMDB || Insn->id == ARM_INS_FSTMDBX);
    unsigned Total = 0;
    for (int I = 1; I < ARM.op_count; I++)
      Total += L.operandWrite(ARM.operands[I]).Size;
    int Cur = IsDB ? -static_cast<int>(Total) : 0;
    for (int I = 1; I < ARM.op_count; I++) {
      NdVar Src = L.operandRead(S, ARM.operands[I]);
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Cur)), 4)});
      S.emit(NdOp::STORE, {}, {EA, Src});
      Cur += static_cast<int>(Src.Size);
    }
    if (Insn->detail->writeback && ARM.operands[0].type == ARM_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
      NdVar BaseReg = NdVar::reg(RI.Offset, 4);
      S.emit(IsDB ? NdOp::INT_SUB : NdOp::INT_ADD, BaseReg,
             {BaseReg, NdVar::cst(Total, 4)});
    }
    break;
  }
  case ARM_INS_VLLDM:
  case ARM_INS_VLSTM:
    S.emitIntrinsic(Intrinsic::ArmVscclrm);
    break;
  case ARM_INS_VSCCLRM:
    S.emitIntrinsic(Intrinsic::ArmVscclrm);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
