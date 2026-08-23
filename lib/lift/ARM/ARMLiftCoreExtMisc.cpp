//===- ARMLiftCoreExtMisc.cpp - ARM32 remaining integer instruction lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// CRC32 accumulation, the ARMv8.1-M conditional selects, the MVE
/// long shifts and the TBB/TBH table branches.
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

bool liftCoreExtMisc(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM) {
  switch (Insn->id) {
  // CRC32 — checksum accumulate (polynomial, not plain XOR): Rd = crc(Rn, Rm).
  // The b/h forms consume the low 8/16 bits of Rm.  The bare no-operand form
  // dropped both inputs and the result, folding callers to a constant 0; route
  // through the (accumulator, data) intrinsic like the AArch64 path.
  case ARM_INS_CRC32B:
  case ARM_INS_CRC32H:
  case ARM_INS_CRC32W:
  case ARM_INS_CRC32CB:
  case ARM_INS_CRC32CH:
  case ARM_INS_CRC32CW: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case ARM_INS_CRC32B:
      Id = Intrinsic::ArmCrc32b;
      break;
    case ARM_INS_CRC32H:
      Id = Intrinsic::ArmCrc32h;
      break;
    case ARM_INS_CRC32W:
      Id = Intrinsic::ArmCrc32w;
      break;
    case ARM_INS_CRC32CB:
      Id = Intrinsic::ArmCrc32cb;
      break;
    case ARM_INS_CRC32CH:
      Id = Intrinsic::ArmCrc32ch;
      break;
    default:
      Id = Intrinsic::ArmCrc32cw;
      break;
    }
    S.emitIntrinsic(Id, Dst, {A, B});
    break;
  }

  // Conditional select (ARMv8.1-M): CSEL/CSINC/CSINV/CSNEG
  case ARM_INS_CSEL:
  case ARM_INS_CSINC:
  case ARM_INS_CSINV:
  case ARM_INS_CSNEG: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    // Build condition from ARM.cc
    NdVar Cond = L.buildCondCode(ARM.cc, S);
    NdVar TrueVal = L.operandRead(S, ARM.operands[1]);
    NdVar FalseVal;
    NdVar Src2 = L.operandRead(S, ARM.operands[2]);
    switch (Insn->id) {
    case ARM_INS_CSEL:
      FalseVal = Src2;
      break;
    case ARM_INS_CSINC:
      FalseVal = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, FalseVal, {Src2, NdVar::cst(1, 4)});
      break;
    case ARM_INS_CSINV:
      FalseVal = S.makeTemp(4);
      S.emit(NdOp::INT_NOT, FalseVal, {Src2});
      break;
    case ARM_INS_CSNEG:
      FalseVal = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, FalseVal, {Src2});
      break;
    default:
      FalseVal = Src2;
      break;
    }
    S.emit(NdOp::SELECT, Dst, {Cond, TrueVal, FalseVal});
    break;
  }

  // Shifts (MVE)
  case ARM_INS_ASRL:
  case ARM_INS_LSLL:
  case ARM_INS_LSRL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdOp Opc = NdOp::INT_ASHR;
    if (Insn->id == ARM_INS_LSLL)
      Opc = NdOp::INT_LEFT;
    else if (Insn->id == ARM_INS_LSRL)
      Opc = NdOp::INT_RIGHT;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case ARM_INS_SQSHL:
  case ARM_INS_SQSHLL:
  case ARM_INS_UQSHL:
  case ARM_INS_UQSHLL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    if (ARM.op_count >= 3) {
      NdVar Amt = L.operandRead(S, ARM.operands[2]);
      S.emit(NdOp::INT_LEFT, Dst, {Src, Amt});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_UQRSHL:
  case ARM_INS_UQRSHLL:
  case ARM_INS_SQRSHR:
  case ARM_INS_SQRSHRL:
  case ARM_INS_SRSHR:
  case ARM_INS_SRSHRL:
  case ARM_INS_URSHR:
  case ARM_INS_URSHRL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    if (ARM.op_count >= 3) {
      NdVar Amt = L.operandRead(S, ARM.operands[2]);
      S.emit(NdOp::INT_RIGHT, Dst, {Src, Amt});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // Table branch
  case ARM_INS_TBB:
  case ARM_INS_TBH: {
    if (ARM.op_count < 1 || ARM.operands[0].type != ARM_OP_MEM)
      break;

    // TBB/TBH do not load a pointer-sized branch target.  They load an
    // unsigned byte/halfword offset from [base,index{,lsl #1}], multiply it by
    // two, and add it to the architectural PC value.  Treating the memory
    // operand through operandRead() used to emit a 4-byte LOAD followed by a
    // direct INDIR_BR of those table bytes, which both mis-modeled execution
    // and erased the exact index operand needed by jump-table guard analysis.
    // Most Thumb instructions read PC as Align(address + 4, 4), and the
    // instruction prologue deliberately installs that value in the synthetic
    // PC register.  TBB/TBH are the exception: their Rn==PC table base and
    // branch target base both use the raw Thumb PC (address + 4), including
    // when the 32-bit instruction starts at the second halfword of a word.
    // Override PC only for this terminating instruction; the next instruction
    // installs its own architectural PC value as usual.  Keep the arithmetic
    // explicitly 32-bit so an instruction at the top of the address space
    // wraps like the architecture.
    const uint32_t RawThumbPC =
        static_cast<uint32_t>(static_cast<uint32_t>(S.Addr) + uint32_t{4});
    S.emit(NdOp::COPY, NdVar::reg(armreg::PC, 4),
           {NdVar::address(RawThumbPC, 4)});

    const uint16_t EntryWidth = Insn->id == ARM_INS_TBB ? 1 : 2;
    NdVar Address = L.operandEffAddr(S, ARM.operands[0]);
    NdVar Entry = S.makeTemp(EntryWidth);
    NdVar Extended = S.makeTemp(4);
    NdVar Scaled = S.makeTemp(4);
    NdVar Target = S.makeTemp(4);
    S.emit(NdOp::LOAD, Entry, {Address});
    S.emit(NdOp::INT_ZEXT, Extended, {Entry});
    S.emit(NdOp::INT_LEFT, Scaled, {Extended, NdVar::scalar(1, 4)});
    S.emit(NdOp::INT_ADD, Target, {NdVar::reg(armreg::PC, 4), Scaled});
    S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
