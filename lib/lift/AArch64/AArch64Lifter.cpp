//===- AArch64Lifter.cpp - AArch64 lifter dispatch & helpers ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Main dispatch for AArch64 instruction lifting, plus operand helpers.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/AArch64Lifter.h"

#include "neverd/decode/AArch64FastClassify.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

// ===----------------------------------------------------------------------===//
// AArch64Lifter construction
// ===----------------------------------------------------------------------===//

AArch64Lifter::AArch64Lifter(Arch A) : TargetArch(A) {}

// ===----------------------------------------------------------------------===//
// Operand read / write
// ===----------------------------------------------------------------------===//

NdVar AArch64Lifter::operandRead(LiftState &S, const cs_aarch64_op &Op) {
  switch (Op.type) {
  case AARCH64_OP_REG: {
    auto RI = mapCapstoneReg(static_cast<aarch64_reg>(Op.reg));
    if (RI.Size == 0)
      return NdVar::cst(0, 8);
    if (RI.Offset == a64reg::XZR)
      return NdVar::cst(0, RI.Size);
    NdVar RegVal = NdVar::reg(RI.Offset, RI.Size);

    if (Op.vector_index >= 0) {
      unsigned ElemSz = 0;
      switch (Op.vas) {
      case AARCH64LAYOUT_VL_B:
        ElemSz = 1;
        break;
      case AARCH64LAYOUT_VL_H:
        ElemSz = 2;
        break;
      case AARCH64LAYOUT_VL_S:
        ElemSz = 4;
        break;
      case AARCH64LAYOUT_VL_D:
        ElemSz = 8;
        break;
      default:
        break;
      }
      if (ElemSz > 0) {
        unsigned ByteOff = Op.vector_index * ElemSz;
        // vector_index is only checked as >= 0; a malformed encoding can index
        // a lane past the register.  Only extract when the element is fully in
        // range, otherwise fall through to a whole-register read so the
        // SUBBYTES never reads out of bounds.
        if (ByteOff + ElemSz <= RI.Size) {
          NdVar Elem = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Elem, {RegVal, NdVar::cst(ByteOff, 4)});
          return Elem;
        }
      }
    }

    // Extended-register operand: <Wm|Xm>, <extend> {#amount}, e.g.
    //   add x0, x1, w2, sxtw   /   add x0, x1, w2, uxtb #3
    // The register's low ExtractSz bytes are sign/zero-extended to 64 bits and
    // then optionally left-shifted.  Ignoring the extend (the previous
    // behaviour) leaks the register's high bits / wrong sign into the result.
    if (Op.ext != AARCH64_EXT_INVALID) {
      unsigned ExtractSz = 8;
      bool Signed = false;
      switch (Op.ext) {
      case AARCH64_EXT_SXTB:
        ExtractSz = 1;
        Signed = true;
        break;
      case AARCH64_EXT_SXTH:
        ExtractSz = 2;
        Signed = true;
        break;
      case AARCH64_EXT_SXTW:
        ExtractSz = 4;
        Signed = true;
        break;
      case AARCH64_EXT_SXTX:
        ExtractSz = 8;
        Signed = true;
        break;
      case AARCH64_EXT_UXTB:
        ExtractSz = 1;
        break;
      case AARCH64_EXT_UXTH:
        ExtractSz = 2;
        break;
      case AARCH64_EXT_UXTW:
        ExtractSz = 4;
        break;
      case AARCH64_EXT_UXTX:
        ExtractSz = 8;
        break;
      default:
        break;
      }
      NdVar Ext;
      if (ExtractSz >= 8) {
        // Full 64-bit: take the X register view as-is.
        Ext = NdVar::reg(RI.Offset, 8);
      } else {
        NdVar Narrow = S.makeTemp(ExtractSz);
        S.emit(NdOp::SUBBYTES, Narrow, {RegVal, NdVar::cst(0, 4)});
        Ext = S.makeTemp(8);
        S.emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ext, {Narrow});
      }
      if (Op.shift.value != 0) {
        NdVar Shifted = S.makeTemp(8);
        S.emit(NdOp::INT_LEFT, Shifted, {Ext, NdVar::cst(Op.shift.value, 8)});
        return Shifted;
      }
      return Ext;
    }

    if (Op.shift.type != AARCH64_SFT_INVALID && Op.shift.value != 0) {
      NdVar Shifted = S.makeTemp(RI.Size);
      NdOp ShiftOp = NdOp::INT_LEFT;
      switch (Op.shift.type) {
      case AARCH64_SFT_LSL:
        ShiftOp = NdOp::INT_LEFT;
        break;
      case AARCH64_SFT_LSR:
        ShiftOp = NdOp::INT_RIGHT;
        break;
      case AARCH64_SFT_ASR:
        ShiftOp = NdOp::INT_ASHR;
        break;
      case AARCH64_SFT_ROR: {
        // Logical shifted-register ROR (`eor xD,xN,xM,ror #s`): rotate right by
        // a constant 0<s<width.  No dedicated rotate op, so OR the two halves.
        uint16_t Bits = static_cast<uint16_t>(RI.Size * 8);
        NdVar Hi = S.makeTemp(RI.Size);
        S.emit(NdOp::INT_RIGHT, Hi,
               {RegVal, NdVar::cst(Op.shift.value, RI.Size)});
        NdVar Lo = S.makeTemp(RI.Size);
        S.emit(NdOp::INT_LEFT, Lo,
               {RegVal, NdVar::cst(Bits - Op.shift.value, RI.Size)});
        S.emit(NdOp::INT_OR, Shifted, {Hi, Lo});
        return Shifted;
      }
      default:
        return RegVal;
      }
      S.emit(ShiftOp, Shifted, {RegVal, NdVar::cst(Op.shift.value, RI.Size)});
      return Shifted;
    }
    return RegVal;
  }
  case AARCH64_OP_IMM: {
    uint64_t Val = static_cast<uint64_t>(Op.imm);
    // Honour a shifted immediate (`add xD, xN, #imm, lsl #12`): the shift is
    // part of the operand's value.  The REG branch already applies its shift,
    // but the IMM branch used to drop it, so large ADD/SUB/etc. immediates were
    // off by the 12-bit shift (e.g. +0x20 instead of +0x20000).  MOVZ/MOVN/MOVK
    // compute the immediate themselves and do not route through here.
    if (Op.shift.type == AARCH64_SFT_LSL && Op.shift.value != 0)
      Val <<= Op.shift.value;
    uint16_t Sz = (Val <= UINT32_MAX) ? 4 : 8;
    return NdVar::cst(Val, Sz);
  }
  case AARCH64_OP_MEM: {
    NdVar EA = S.makeTemp(8);
    bool First = true;
    auto Acc = [&](NdVar V) {
      if (First) {
        S.emit(NdOp::COPY, EA, {V});
        First = false;
      } else {
        S.emit(NdOp::INT_ADD, EA, {EA, V});
      }
    };

    if (Op.mem.base != AARCH64_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<aarch64_reg>(Op.mem.base));
      Acc(NdVar::reg(RI.Offset, 8));
    }
    if (Op.mem.index != AARCH64_REG_INVALID)
      Acc(emitMemIndex(S, Op));
    if (Op.mem.disp != 0)
      Acc(NdVar::cst(static_cast<uint64_t>(Op.mem.disp), 8));
    if (First)
      Acc(NdVar::cst(0, 8));

    uint16_t Sz = 8;
    NdVar Result = S.makeTemp(Sz);
    S.emit(NdOp::LOAD, Result, {EA});
    return Result;
  }
  case AARCH64_OP_FP: {
    double FPVal = Op.fp;
    uint64_t Bits;
    std::memcpy(&Bits, &FPVal, 8);
    return NdVar::cst(Bits, 8);
  }
  default:
    return NdVar::cst(0, 8);
  }
}

NdVar AArch64Lifter::narrowToWidth(LiftState &S, NdVar V, uint16_t Sz) {
  if (Sz == 0 || V.Size <= Sz)
    return V;
  NdVar Narrow = S.makeTemp(Sz);
  S.emit(NdOp::SUBBYTES, Narrow, {V, NdVar::cst(0, 4)});
  return Narrow;
}

NdVar AArch64Lifter::emitMemIndex(LiftState &S, const cs_aarch64_op &Op) {
  auto RI = mapCapstoneReg(static_cast<aarch64_reg>(Op.mem.index));
  // UXTW/SXTW extend the 32-bit W view of the index to 64 bits.  Checking the
  // LSL shift first (the old behaviour) read the whole 64-bit X register and
  // shifted it, dropping the extend — a negative 32-bit (sxtw) index whose high
  // half is zero then became a huge positive offset instead of a negative one.
  // (S|U)XTX / no extend use the full X register.
  NdVar IdxVal;
  bool Ext32 = (Op.ext == AARCH64_EXT_SXTW || Op.ext == AARCH64_EXT_UXTW);
  bool Signed = (Op.ext == AARCH64_EXT_SXTW);
  if (Ext32) {
    IdxVal = S.makeTemp(8);
    S.emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, IdxVal,
           {NdVar::reg(RI.Offset, 4)});
  } else {
    IdxVal = NdVar::reg(RI.Offset, 8);
  }
  // For a memory index a non-zero shift amount is always the LSL scale (the
  // scaled-extend form carries it in shift.value with shift.type==LSL).
  if (Op.shift.value != 0) {
    NdVar Shifted = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, Shifted, {IdxVal, NdVar::cst(Op.shift.value, 8)});
    return Shifted;
  }
  return IdxVal;
}

NdVar AArch64Lifter::operandEffAddr(LiftState &S, const cs_aarch64_op &Op) {
  if (Op.type != AARCH64_OP_MEM) {
    // Register-indirect addressing (`[xN]` capstone may surface as a REG): the
    // operand value itself is the address.  Anything else: read it directly.
    return operandRead(S, Op);
  }
  NdVar EA = S.makeTemp(8);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, EA, {V});
      First = false;
    } else {
      S.emit(NdOp::INT_ADD, EA, {EA, V});
    }
  };
  if (Op.mem.base != AARCH64_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<aarch64_reg>(Op.mem.base));
    Acc(NdVar::reg(RI.Offset, 8));
  }
  if (Op.mem.index != AARCH64_REG_INVALID)
    Acc(emitMemIndex(S, Op));
  if (Op.mem.disp != 0)
    Acc(NdVar::cst(static_cast<uint64_t>(Op.mem.disp), 8));
  if (First)
    Acc(NdVar::cst(0, 8));
  return EA;
}

NdVar AArch64Lifter::operandWrite(const cs_aarch64_op &Op) {
  if (Op.type == AARCH64_OP_REG) {
    auto RI = mapCapstoneReg(static_cast<aarch64_reg>(Op.reg));
    if (RI.Size == 0)
      return NdVar::cst(0, 8);
    if (RI.Offset == a64reg::XZR)
      return NdVar::tmp(DiscardXzr32, RI.Size);
    return NdVar::reg(RI.Offset, RI.Size);
  }
  return NdVar::tmp(DiscardXzr64, 8);
}

// ===----------------------------------------------------------------------===//
// Main dispatch
// ===----------------------------------------------------------------------===//

void AArch64Lifter::lift(const cs_insn *Insn, std::vector<LowOp> &Ops) {
  auto *Detail = Insn->detail;
  if (!Detail)
    return;

  auto &ARM64 = Detail->aarch64;
  LiftState S(Insn->address, static_cast<uint16_t>(Insn->size), Ops);

  bool Handled = liftCore(S, Insn, ARM64) || liftCoreNEON(S, Insn, ARM64) ||
                 liftControl(S, Insn, ARM64) || liftMem(S, Insn, ARM64) ||
                 liftAtomic(S, Insn, ARM64) || liftFP(S, Insn, ARM64) ||
                 liftSIMD(S, Insn, ARM64) || liftSIMDExt(S, Insn, ARM64);

  if (!Handled) {
    if (Strict)
      throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
    S.emit(NdOp::NOP, {}, {});
    LLVM_DEBUG(llvm::dbgs()
               << "Unlifted AArch64 instruction: " << Insn->mnemonic << " "
               << Insn->op_str << " @ 0x" << llvm::utohexstr(S.Addr) << "\n");
  }

  // W/X aliasing: writing to Wn zeros upper 32 Bits of Xn.
  llvm::DenseSet<uint64_t> XWritten;
  for (size_t K = S.OpsStart; K < Ops.size(); ++K) {
    auto &O = Ops[K];
    if (O.Output.isReg() && O.Output.Size == 8 && O.Output.Offset < a64reg::X29)
      XWritten.insert(O.Output.Offset);
  }
  size_t OpsEnd = Ops.size();
  for (size_t K = S.OpsStart; K < OpsEnd; ++K) {
    // Copy the fields out before S.emit(): emit() appends to Ops, which can
    // reallocate its backing store and leave the reference O dangling.  Reading
    // O.Output.* after emit() would then be a use-after-free (it surfaced as a
    // garbage offset equal to the DenseSet sentinel, tripping an assert).
    uint64_t OutOff = Ops[K].Output.Offset;
    if (Ops[K].Output.isReg() && Ops[K].Output.Size == 4 &&
        OutOff < a64reg::X29 && !XWritten.count(OutOff)) {
      S.emit(NdOp::INT_ZEXT, NdVar::reg(OutOff, 8),
             {NdVar::reg(OutOff, 4)});
      XWritten.insert(OutOff);
    }
  }

  // D/Q aliasing: writing to Dn (8 bytes) zeros upper 64 bits of Qn.
  // .8B, .4H, .2S arrangements write to D-sized registers; Q must be zeroed.
  llvm::DenseSet<uint64_t> QWritten;
  for (size_t K = S.OpsStart; K < Ops.size(); ++K) {
    auto &O = Ops[K];
    if (O.Output.isReg() && O.Output.Size == 16 &&
        O.Output.Offset >= a64reg::V0 && O.Output.Offset < a64reg::V0 + 32 * 16)
      QWritten.insert(O.Output.Offset);
  }
  OpsEnd = Ops.size();
  for (size_t K = S.OpsStart; K < OpsEnd; ++K) {
    auto &O = Ops[K];
    if (O.Output.isReg() && O.Output.Offset >= a64reg::V0 &&
        O.Output.Offset < a64reg::V0 + 32 * 16 && O.Output.Size < 16) {
      uint64_t QOff = a64reg::V0 + ((O.Output.Offset - a64reg::V0) / 16) * 16;
      if (!QWritten.count(QOff)) {
        S.emit(NdOp::INT_ZEXT, NdVar::reg(QOff, 16),
               {NdVar::reg(O.Output.Offset, O.Output.Size)});
        QWritten.insert(QOff);
      }
    }
  }
}

// ===----------------------------------------------------------------------===//
// Decode-time instruction classification
// ===----------------------------------------------------------------------===//

void AArch64Lifter::fixupDecodedInsn(cs_insn * /*I*/) {
  // No capstone decode-id quirks to correct for AArch64 (yet).
}

bool AArch64Lifter::isFunctionTerminator(const cs_insn *I) {
  switch (I->id) {
  case AARCH64_INS_RET:
  case AARCH64_INS_B:
  case AARCH64_INS_BR:
  case AARCH64_INS_ERET:
    return true;
  default:
    return false;
  }
}

va_t AArch64Lifter::directCallTarget(const cs_insn *I) {
  if (!I->detail)
    return InvalidVA;
  const cs_aarch64 &A = I->detail->aarch64;
  if (I->id == AARCH64_INS_BL && A.op_count >= 1 &&
      A.operands[0].type == AARCH64_OP_IMM)
    return static_cast<va_t>(A.operands[0].imm);
  return InvalidVA;
}

va_t AArch64Lifter::decodeBranchLinkTarget(uint32_t Word, va_t Addr) {
  // Single source of truth for the fixed-width BL classification, shared with
  // the front-end scans that bypass Capstone (see AArch64FastClassify.h).
  return a64fast::directCallTarget(Word, Addr);
}

} // namespace neverd
