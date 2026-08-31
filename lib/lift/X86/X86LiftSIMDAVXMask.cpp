//===- X86LiftSIMDAVXMask.cpp - x86/x64 AVX-512 opmask register lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AVX-512 opmask (k0-k7) register operations: bitwise
/// logic, shifts, tests, unpack, add and move.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

enum class ApxKmovValidation { NotApxEvex, Valid, Invalid };

int apxKmovGprIndex(x86_reg Reg, unsigned Width) {
  static const x86_reg Low32[] = {X86_REG_EAX, X86_REG_ECX, X86_REG_EDX,
                                  X86_REG_EBX, X86_REG_ESP, X86_REG_EBP,
                                  X86_REG_ESI, X86_REG_EDI};
  static const x86_reg Low64[] = {X86_REG_RAX, X86_REG_RCX, X86_REG_RDX,
                                  X86_REG_RBX, X86_REG_RSP, X86_REG_RBP,
                                  X86_REG_RSI, X86_REG_RDI};
  if (Width != 4 && Width != 8)
    return -1;
  const x86_reg *Low = Width == 4 ? Low32 : Low64;
  for (unsigned I = 0; I != 8; ++I)
    if (Reg == Low[I])
      return static_cast<int>(I);
  if (Width == 4) {
    if (Reg >= X86_REG_R8D && Reg <= X86_REG_R15D)
      return 8 + static_cast<int>(Reg - X86_REG_R8D);
    if (Reg >= X86_REG_R16D && Reg <= X86_REG_R31D)
      return 16 + static_cast<int>(Reg - X86_REG_R16D);
  } else {
    if (Reg >= X86_REG_R8 && Reg <= X86_REG_R15)
      return 8 + static_cast<int>(Reg - X86_REG_R8);
    if (Reg >= X86_REG_R16 && Reg <= X86_REG_R31)
      return 16 + static_cast<int>(Reg - X86_REG_R16);
  }
  return -1;
}

x86_reg apxKmovSegment(uint8_t Prefix) {
  switch (Prefix) {
  case 0x26:
    return X86_REG_ES;
  case 0x2e:
    return X86_REG_CS;
  case 0x36:
    return X86_REG_SS;
  case 0x3e:
    return X86_REG_DS;
  case 0x64:
    return X86_REG_FS;
  case 0x65:
    return X86_REG_GS;
  default:
    return X86_REG_INVALID;
  }
}

bool apxKmovOperandIsRegister(const cs_x86_op &Operand, x86_reg Reg,
                              unsigned Size, uint8_t Access) {
  return Operand.type == X86_OP_REG && Operand.reg == Reg &&
         Operand.size == Size && Operand.access == Access;
}

ApxKmovValidation validateApxKmov(const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn || !Insn->detail || Insn->size == 0 || Insn->size > 15)
    return ApxKmovValidation::Invalid;

  size_t EvexOffset = 0;
  uint8_t SegmentPrefix = 0;
  bool Address32 = false;
  bool DuplicatePrefix = false;
  while (EvexOffset < Insn->size) {
    const uint8_t Prefix = Insn->bytes[EvexOffset];
    if (Prefix == 0x67) {
      DuplicatePrefix |= Address32;
      Address32 = true;
      ++EvexOffset;
      continue;
    }
    if (apxKmovSegment(Prefix) != X86_REG_INVALID) {
      DuplicatePrefix |= SegmentPrefix != 0;
      SegmentPrefix = Prefix;
      ++EvexOffset;
      continue;
    }
    break;
  }

  // Preserve the existing VEX KMOV path verbatim, but never let a decoded
  // APX-EVEX instruction escape validation after its raw prefix is mutated.
  // Capstone records the complete APX EVEX prefix in opcode[0..3], so byte 0
  // is a stable presence marker even when Insn->bytes no longer agrees.
  const bool DetailIsApxEvex = X86.opcode[0] == 0x62;
  if (EvexOffset < Insn->size &&
      (Insn->bytes[EvexOffset] == 0xc4 || Insn->bytes[EvexOffset] == 0xc5))
    return DetailIsApxEvex ? ApxKmovValidation::Invalid
                           : ApxKmovValidation::NotApxEvex;
  if (DuplicatePrefix || EvexOffset + 6 > Insn->size ||
      Insn->bytes[EvexOffset] != 0x62 || !DetailIsApxEvex || X86.op_count != 2)
    return ApxKmovValidation::Invalid;

  const uint8_t P0 = Insn->bytes[EvexOffset + 1];
  const uint8_t P1 = Insn->bytes[EvexOffset + 2];
  const uint8_t P2 = Insn->bytes[EvexOffset + 3];
  const uint8_t Opcode = Insn->bytes[EvexOffset + 4];
  const size_t ModRMOffset = EvexOffset + 5;
  const uint8_t ModRM = Insn->bytes[ModRMOffset];
  const bool Memory = (ModRM & 0xc0) != 0xc0;
  if ((P0 & 7) != 1 || Opcode < 0x90 || Opcode > 0x93 || (P1 & 0x78) != 0x78 ||
      (!Memory && (P1 & 4) == 0) || P2 != 8 ||
      X86.encoding.modrm_offset != ModRMOffset ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.modrm != ModRM || X86.addr_size != (Address32 ? 4 : 8) ||
      X86.prefix[0] != 0 || X86.prefix[1] != SegmentPrefix ||
      X86.prefix[2] != 0 || X86.prefix[3] != (Address32 ? 0x67 : 0) ||
      X86.eflags != 0 || X86.avx_cc != X86_AVX_CC_INVALID || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || Insn->detail->regs_read_count != 0 ||
      Insn->detail->regs_write_count != 0)
    return ApxKmovValidation::Invalid;
  for (unsigned I = 0; I != 4; ++I)
    if (X86.opcode[I] != Insn->bytes[EvexOffset + I])
      return ApxKmovValidation::Invalid;
  for (unsigned I = 0; I != X86.op_count; ++I)
    if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[I].avx_zero_opmask)
      return ApxKmovValidation::Invalid;

  const uint8_t PP = P1 & 3;
  const bool W = (P1 & 0x80) != 0;
  unsigned Width = 0;
  if (Opcode <= 0x91) {
    if (PP == 1 && !W)
      Width = 1;
    else if (PP == 0 && !W)
      Width = 2;
    else if (PP == 1 && W)
      Width = 4;
    else if (PP == 0 && W)
      Width = 8;
  } else {
    if (PP == 1 && !W)
      Width = 1;
    else if (PP == 0 && !W)
      Width = 2;
    else if (PP == 3 && !W)
      Width = 4;
    else if (PP == 3 && W)
      Width = 8;
  }
  const unsigned ExpectedId = Width == 1   ? X86_INS_KMOVB
                              : Width == 2 ? X86_INS_KMOVW
                              : Width == 4 ? X86_INS_KMOVD
                              : Width == 8 ? X86_INS_KMOVQ
                                           : X86_INS_INVALID;
  if (ExpectedId == X86_INS_INVALID || Insn->id != ExpectedId ||
      (Opcode == 0x91 && !Memory) ||
      ((Opcode == 0x92 || Opcode == 0x93) && Memory))
    return ApxKmovValidation::Invalid;

  const unsigned RegNumber =
      ((~P0 & 0x80) >> 4) | (~P0 & 0x10) | ((ModRM >> 3) & 7);
  const unsigned RMNumber = ((~P0 & 0x20) >> 2) | ((P0 & 8) << 1) | (ModRM & 7);
  const unsigned GprWidth = Width == 8 ? 8 : 4;
  x86_reg ExactRegField = Opcode != 0x93 && RegNumber <= 7
                              ? static_cast<x86_reg>(X86_REG_K0 + RegNumber)
                              : X86_REG_INVALID;
  if (Opcode == 0x93) {
    ExactRegField = static_cast<x86_reg>(X86.operands[0].reg);
    if (apxKmovGprIndex(ExactRegField, GprWidth) != static_cast<int>(RegNumber))
      return ApxKmovValidation::Invalid;
  }
  if (ExactRegField == X86_REG_INVALID)
    return ApxKmovValidation::Invalid;

  if (!Memory) {
    if (Insn->size != ModRMOffset + 1 || X86.encoding.disp_size != 0 ||
        X86.encoding.disp_offset != 0 || X86.disp != 0 || X86.sib != 0 ||
        X86.sib_base != X86_REG_INVALID || X86.sib_index != X86_REG_INVALID ||
        X86.sib_scale != 0)
      return ApxKmovValidation::Invalid;
    if (Opcode == 0x90) {
      const x86_reg RMField = RMNumber <= 7
                                  ? static_cast<x86_reg>(X86_REG_K0 + RMNumber)
                                  : X86_REG_INVALID;
      if (!apxKmovOperandIsRegister(X86.operands[0], ExactRegField, Width,
                                    CS_AC_WRITE) ||
          !apxKmovOperandIsRegister(X86.operands[1], RMField, Width,
                                    CS_AC_READ))
        return ApxKmovValidation::Invalid;
    } else if (Opcode == 0x92) {
      const x86_reg RMField = static_cast<x86_reg>(X86.operands[1].reg);
      if (apxKmovGprIndex(RMField, GprWidth) != static_cast<int>(RMNumber) ||
          !apxKmovOperandIsRegister(X86.operands[0], ExactRegField, Width,
                                    CS_AC_WRITE) ||
          !apxKmovOperandIsRegister(X86.operands[1], RMField, GprWidth,
                                    CS_AC_READ))
        return ApxKmovValidation::Invalid;
    } else if (Opcode == 0x93) {
      const x86_reg RMField = RMNumber <= 7
                                  ? static_cast<x86_reg>(X86_REG_K0 + RMNumber)
                                  : X86_REG_INVALID;
      if (!apxKmovOperandIsRegister(X86.operands[0], ExactRegField, GprWidth,
                                    CS_AC_WRITE) ||
          !apxKmovOperandIsRegister(X86.operands[1], RMField, Width,
                                    CS_AC_READ))
        return ApxKmovValidation::Invalid;
    } else {
      return ApxKmovValidation::Invalid;
    }
    return ApxKmovValidation::Valid;
  }

  if (RegNumber > 7)
    return ApxKmovValidation::Invalid;
  const x86_reg KReg = static_cast<x86_reg>(X86_REG_K0 + RegNumber);
  const unsigned MemoryIndex = Opcode == 0x91 ? 0 : 1;
  const unsigned RegisterIndex = Opcode == 0x91 ? 1 : 0;
  const auto &MemoryOperand = X86.operands[MemoryIndex];
  if (!apxKmovOperandIsRegister(X86.operands[RegisterIndex], KReg, Width,
                                Opcode == 0x91 ? CS_AC_READ : CS_AC_WRITE) ||
      MemoryOperand.type != X86_OP_MEM || MemoryOperand.size != Width ||
      MemoryOperand.access != (Opcode == 0x91 ? CS_AC_WRITE : CS_AC_READ))
    return ApxKmovValidation::Invalid;

  const unsigned AddressWidth = Address32 ? 4 : 8;
  const unsigned BaseExtension = ((~P0 & 0x20) >> 2) | ((P0 & 8) << 1);
  const unsigned IndexExtension = ((~P0 & 0x40) >> 3) | ((~P1 & 4) << 2);
  const unsigned Mod = ModRM >> 6;
  const unsigned RM = ModRM & 7;
  size_t Cursor = ModRMOffset + 1;
  x86_reg ExpectedBase = X86_REG_INVALID;
  x86_reg ExpectedIndex = X86_REG_INVALID;
  unsigned ExpectedScale = 1;
  uint8_t ExpectedDispSize = 0;
  bool HasSIB = false;
  uint8_t SIB = 0;
  if (RM == 4) {
    if (Cursor >= Insn->size)
      return ApxKmovValidation::Invalid;
    HasSIB = true;
    SIB = Insn->bytes[Cursor++];
    ExpectedScale = 1u << (SIB >> 6);
    const unsigned Index = (SIB >> 3) & 7;
    const unsigned Base = SIB & 7;
    if (Index != 4 || IndexExtension != 0) {
      ExpectedIndex = static_cast<x86_reg>(MemoryOperand.mem.index);
      if (apxKmovGprIndex(ExpectedIndex, AddressWidth) !=
          static_cast<int>(Index + IndexExtension))
        return ApxKmovValidation::Invalid;
    }
    if (Mod == 0 && Base == 5)
      ExpectedDispSize = 4;
    else {
      ExpectedBase = static_cast<x86_reg>(MemoryOperand.mem.base);
      if (apxKmovGprIndex(ExpectedBase, AddressWidth) !=
          static_cast<int>(Base + BaseExtension))
        return ApxKmovValidation::Invalid;
    }
  } else if (Mod == 0 && RM == 5) {
    ExpectedBase = Address32 ? X86_REG_EIP : X86_REG_RIP;
    ExpectedDispSize = 4;
  } else {
    ExpectedBase = static_cast<x86_reg>(MemoryOperand.mem.base);
    if (apxKmovGprIndex(ExpectedBase, AddressWidth) !=
        static_cast<int>(RM + BaseExtension))
      return ApxKmovValidation::Invalid;
  }
  if (Mod == 1)
    ExpectedDispSize = 1;
  else if (Mod == 2)
    ExpectedDispSize = 4;

  int64_t ExpectedDisp = 0;
  const size_t DispOffset = Cursor;
  if (ExpectedDispSize == 1) {
    if (Cursor >= Insn->size)
      return ApxKmovValidation::Invalid;
    ExpectedDisp = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (ExpectedDispSize == 4) {
    if (Cursor + 4 > Insn->size)
      return ApxKmovValidation::Invalid;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 4; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor++]) << (I * 8);
    ExpectedDisp = static_cast<int32_t>(Raw);
  }
  if (Cursor != Insn->size ||
      MemoryOperand.mem.segment != apxKmovSegment(SegmentPrefix) ||
      MemoryOperand.mem.base != ExpectedBase ||
      MemoryOperand.mem.index != ExpectedIndex ||
      MemoryOperand.mem.scale != static_cast<int>(ExpectedScale) ||
      MemoryOperand.mem.disp != ExpectedDisp || X86.disp != ExpectedDisp ||
      X86.encoding.disp_size != ExpectedDispSize ||
      X86.encoding.disp_offset != (ExpectedDispSize ? DispOffset : 0) ||
      X86.sib != (HasSIB ? SIB : 0) ||
      X86.sib_base != (HasSIB ? ExpectedBase : X86_REG_INVALID) ||
      X86.sib_index != (HasSIB ? ExpectedIndex : X86_REG_INVALID) ||
      X86.sib_scale != (HasSIB ? static_cast<int>(ExpectedScale) : 0))
    return ApxKmovValidation::Invalid;
  return ApxKmovValidation::Valid;
}

} // namespace

bool liftSIMDAVXMask(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // P1: AVX-512 k-Mask register operations (opmask k0-k7).
  // ========================================================================

  // KAND{B,W,D,Q} — Mask AND
  case X86_INS_KANDB:
  case X86_INS_KANDW:
  case X86_INS_KANDD:
  case X86_INS_KANDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    break;
  }

  // KANDN{B,W,D,Q} — Mask AND-NOT
  case X86_INS_KANDNB:
  case X86_INS_KANDNW:
  case X86_INS_KANDND:
  case X86_INS_KANDNQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // KOR{B,W,D,Q} — Mask OR
  case X86_INS_KORB:
  case X86_INS_KORW:
  case X86_INS_KORD:
  case X86_INS_KORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }

  // KXOR{B,W,D,Q} — Mask XOR
  case X86_INS_KXORB:
  case X86_INS_KXORW:
  case X86_INS_KXORD:
  case X86_INS_KXORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

  // KXNOR{B,W,D,Q} — Mask XNOR
  case X86_INS_KXNORB:
  case X86_INS_KXNORW:
  case X86_INS_KXNORD:
  case X86_INS_KXNORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Xored = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_XOR, Xored, {A, B});
    S.emit(NdOp::INT_NOT, Dst, {Xored});
    break;
  }

  // KNOT{B,W,D,Q} — Mask NOT
  case X86_INS_KNOTB:
  case X86_INS_KNOTW:
  case X86_INS_KNOTD:
  case X86_INS_KNOTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
    break;
  }

  // KMOV{B,W,D,Q} — Mask move
  case X86_INS_KMOVB:
  case X86_INS_KMOVW:
  case X86_INS_KMOVD:
  case X86_INS_KMOVQ: {
    const ApxKmovValidation Apx = validateApxKmov(Insn, X86);
    if (Apx == ApxKmovValidation::Invalid)
      return false;
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Src);
    else
      S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Src});
    break;
  }

  // KSHIFTL{B,W,D,Q} — Mask shift left
  case X86_INS_KSHIFTLB:
  case X86_INS_KSHIFTLW:
  case X86_INS_KSHIFTLD:
  case X86_INS_KSHIFTLQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Cnt = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {Src, Cnt});
    break;
  }

  // KSHIFTR{B,W,D,Q} — Mask shift right
  case X86_INS_KSHIFTRB:
  case X86_INS_KSHIFTRW:
  case X86_INS_KSHIFTRD:
  case X86_INS_KSHIFTRQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Cnt = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_RIGHT, Dst, {Src, Cnt});
    break;
  }

  // KTEST{B,W,D,Q} — Mask test (set ZF/CF from AND of two Mask regs).
  case X86_INS_KTESTB:
  case X86_INS_KTESTW:
  case X86_INS_KTESTD:
  case X86_INS_KTESTQ: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar Masked = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Masked, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Masked, NdVar::cst(0, A.Size)});
    NdVar NotA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    NdVar AndNot = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndNot, {NotA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndNot, NdVar::cst(0, A.Size)});
    break;
  }

  // KORTEST{B,W,D,Q} — Mask OR test (ZF = (src1|src2)==0, CF =
  // (src1|src2)==all1s).
  case X86_INS_KORTESTB:
  case X86_INS_KORTESTW:
  case X86_INS_KORTESTD:
  case X86_INS_KORTESTQ: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar Ored = S.makeTemp(A.Size);
    S.emit(NdOp::INT_OR, Ored, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Ored, NdVar::cst(0, A.Size)});
    // CF = 1 iff all Bits of (src1|src2) are set, i.e. ~(ored) == 0.
    NdVar NotOred = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NotOred, {Ored});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {NotOred, NdVar::cst(0, A.Size)});
    break;
  }

  // KUNPCK{BW,WD,DQ} — Mask unpack (concatenate low halves).
  case X86_INS_KUNPCKBW:
  case X86_INS_KUNPCKWD:
  case X86_INS_KUNPCKDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Kunpck, Dst, {A, B});
    break;
  }

  // KADD{B,W,D,Q} — Mask add
  case X86_INS_KADDB:
  case X86_INS_KADDW:
  case X86_INS_KADDD:
  case X86_INS_KADDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
