//===- ARMLiftMemAtomic.cpp - ARM32 exclusive and ordered access lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Load/store exclusive (LDREX*, STREX*), acquire/release (LDA*,
/// STL*), the SWP pair and the unprivileged LDRT/STRT accesses.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMemAtomic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  switch (Insn->id) {
  // ========================================================================
  // Load exclusive / store exclusive / acquire / release
  // ========================================================================
  case ARM_INS_LDREX:
  case ARM_INS_LDREXB:
  case ARM_INS_LDREXH:
  case ARM_INS_LDA:
  case ARM_INS_LDAB:
  case ARM_INS_LDAH:
  case ARM_INS_LDAEX:
  case ARM_INS_LDAEXB:
  case ARM_INS_LDAEXH: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[1])
                     : L.operandRead(S, ARM.operands[1]);
    // Byte/halfword forms access 1/2 bytes and zero-extend into the register;
    // the old code loaded the full register width (4 bytes).
    uint16_t LoadSz = 4;
    if (Insn->id == ARM_INS_LDREXB || Insn->id == ARM_INS_LDAB ||
        Insn->id == ARM_INS_LDAEXB)
      LoadSz = 1;
    else if (Insn->id == ARM_INS_LDREXH || Insn->id == ARM_INS_LDAH ||
             Insn->id == ARM_INS_LDAEXH)
      LoadSz = 2;
    if (LoadSz < 4) {
      NdVar Val = S.makeTemp(LoadSz);
      S.emit(NdOp::LOAD, Val, {EA});
      S.emit(NdOp::INT_ZEXT, Dst, {Val});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case ARM_INS_LDREXD:
  case ARM_INS_LDAEXD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst1 = L.operandWrite(ARM.operands[0]);
    NdVar Dst2 = L.operandWrite(ARM.operands[1]);
    NdVar EA = (ARM.operands[2].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[2])
                     : L.operandRead(S, ARM.operands[2]);
    NdVar EA2 = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
    NdVar V1 = S.makeTemp(4);
    NdVar V2 = S.makeTemp(4);
    S.emit(NdOp::LOAD, V1, {EA});
    S.emit(NdOp::LOAD, V2, {EA2});
    S.emit(NdOp::COPY, Dst1, {V1});
    S.emit(NdOp::COPY, Dst2, {V2});
    break;
  }
  case ARM_INS_STREX:
  case ARM_INS_STREXB:
  case ARM_INS_STREXH:
  case ARM_INS_STLEX:
  case ARM_INS_STLEXB:
  case ARM_INS_STLEXH: {
    if (ARM.op_count < 3)
      break;
    NdVar Status = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STREXB || Insn->id == ARM_INS_STLEXB)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STREXH || Insn->id == ARM_INS_STLEXH)
      StoreSz = 2;
    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }
    NdVar EA = (ARM.operands[2].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[2])
                     : L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    S.emit(NdOp::COPY, Status, {NdVar::cst(0, 4)});
    break;
  }
  case ARM_INS_STREXD:
  case ARM_INS_STLEXD: {
    if (ARM.op_count < 4)
      break;
    NdVar Status = L.operandWrite(ARM.operands[0]);
    NdVar Src1 = L.operandRead(S, ARM.operands[1]);
    NdVar Src2 = L.operandRead(S, ARM.operands[2]);
    NdVar EA = (ARM.operands[3].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[3])
                     : L.operandRead(S, ARM.operands[3]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    S.emit(NdOp::COPY, Status, {NdVar::cst(0, 4)});
    break;
  }
  case ARM_INS_STL:
  case ARM_INS_STLB:
  case ARM_INS_STLH: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM.operands[0]);
    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STLB)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STLH)
      StoreSz = 2;
    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[1])
                     : L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case ARM_INS_SWP:
  case ARM_INS_SWPB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    NdVar EA = (ARM.operands[2].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[2])
                     : L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::LOAD, Dst, {EA});
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }

  // Unprivileged load/store
  case ARM_INS_LDRT:
  case ARM_INS_LDRBT:
  case ARM_INS_LDRHT:
  case ARM_INS_LDRSBT:
  case ARM_INS_LDRSHT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[1])
                     : L.operandRead(S, ARM.operands[1]);
    // Byte/halfword forms access 1/2 bytes; sign- or zero-extend into the
    // register.  The old code loaded the full register width (4 bytes).
    uint16_t LoadSz = 4;
    if (Insn->id == ARM_INS_LDRBT || Insn->id == ARM_INS_LDRSBT)
      LoadSz = 1;
    else if (Insn->id == ARM_INS_LDRHT || Insn->id == ARM_INS_LDRSHT)
      LoadSz = 2;
    bool SignExt = (Insn->id == ARM_INS_LDRSBT || Insn->id == ARM_INS_LDRSHT);
    if (LoadSz < 4) {
      NdVar Val = S.makeTemp(LoadSz);
      S.emit(NdOp::LOAD, Val, {EA});
      S.emit(SignExt ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Val});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case ARM_INS_STRT:
  case ARM_INS_STRBT:
  case ARM_INS_STRHT: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM.operands[0]);
    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STRBT)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STRHT)
      StoreSz = 2;
    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? L.operandEffAddr(S, ARM.operands[1])
                     : L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
