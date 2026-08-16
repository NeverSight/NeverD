//===- AArch64LiftLdStVariant.cpp - Ordered and unprivileged load/store ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Acquire/release, unscaled and unprivileged load variants
/// (LDAPR/LDAPUR/LDTR/LDPSW/LDRAA/LD64B) and the matching store
/// variants (STLLR/STLUR/STTR/ST64B/STILP/SWPP).
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/lift/AArch64Lifter.h"

#include <cstring>

namespace neverd {

// Memory access width for byte/halfword ordered/unscaled/unprivileged
// load/store variants (mnemonic suffix 'b'/'h'); the register operand is always
// W/X but the access (and zero/truncate width) is 1/2 bytes.
static uint16_t orderedAccessWidth(const cs_insn *Insn, uint16_t RegSz) {
  const char *Mn = Insn->mnemonic;
  size_t L = Mn ? std::strlen(Mn) : 0;
  if (L && Mn[L - 1] == 'b')
    return 1;
  if (L && Mn[L - 1] == 'h')
    return 2;
  return RegSz;
}

// Capstone 6 STLURWi/STLURXi forms tag operands as CS_OP_REG|CS_OP_MEM and may
// surface only one AARCH64_OP_MEM operand with a bogus base/index (e.g. stlurb
// w0,[x1] -> base=x0,index=x1).  Recover Rt/Rn/imm9 from the LD/ST unscaled
// encoding: Rt=[4:0], Rn=[9:5], simm9=[20:12].
static bool decodeUnscaledStoreEncoding(const cs_insn *Insn, aarch64_reg &Rt,
                                        aarch64_reg &Rn, int64_t &Imm9) {
  if (Insn->size != 4)
    return false;
  uint32_t Enc;
  std::memcpy(&Enc, Insn->bytes, 4);
  unsigned RtIdx = Enc & 0x1F;
  unsigned RnIdx = (Enc >> 5) & 0x1F;
  unsigned SzField = (Enc >> 30) & 3;
  bool Is64 = (SzField == 3);
  Rt = a64native::gpr(RtIdx, Is64);
  Rn = a64native::gprSP(RnIdx, true);
  Imm9 = (Enc >> 12) & 0x1FF;
  if (Imm9 & 0x100)
    Imm9 |= ~0x1FFLL;
  return true;
}

static NdVar storeSourceFromEncoding(const cs_insn *Insn) {
  aarch64_reg Rt = AARCH64_REG_INVALID;
  aarch64_reg Rn = AARCH64_REG_INVALID;
  int64_t Imm9 = 0;
  if (!decodeUnscaledStoreEncoding(Insn, Rt, Rn, Imm9))
    return NdVar::cst(0, 4);
  auto RI = mapCapstoneReg(Rt);
  if (RI.Size == 0)
    return NdVar::cst(0, 4);
  if (RI.Offset == a64reg::XZR)
    return NdVar::cst(0, RI.Size);
  return NdVar::reg(RI.Offset, RI.Size);
}

static NdVar storeEffAddrFromEncoding(AArch64Lifter::LiftState &S,
                                      const cs_insn *Insn) {
  aarch64_reg Rt = AARCH64_REG_INVALID;
  aarch64_reg Rn = AARCH64_REG_INVALID;
  int64_t Imm9 = 0;
  if (!decodeUnscaledStoreEncoding(Insn, Rt, Rn, Imm9))
    return S.makeTemp(8);
  auto RI = mapCapstoneReg(Rn);
  NdVar EA = S.makeTemp(8);
  S.emit(NdOp::COPY, EA, {NdVar::reg(RI.Offset, 8)});
  if (Imm9 != 0)
    S.emit(NdOp::INT_ADD, EA, {EA, NdVar::cst(static_cast<uint64_t>(Imm9), 8)});
  return EA;
}

bool liftLdStVariant(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // Load variants (LDAPR, LDTR, LDLAR, LDPSW, LDRAA, LDRAB, etc.)
  // ========================================================================
  case AARCH64_INS_LDAPR:
  case AARCH64_INS_LDAPRB:
  case AARCH64_INS_LDAPRH:
  case AARCH64_INS_LDLAR:
  case AARCH64_INS_LDLARB:
  case AARCH64_INS_LDLARH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = orderedAccessWidth(Insn, Dst.Size);
    if (Asz < Dst.Size) {
      NdVar V = S.makeTemp(Asz);
      S.emit(NdOp::LOAD, V, {EA}, NdMemoryOrdering::Acquire);
      S.emit(NdOp::INT_ZEXT, Dst, {V});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA}, NdMemoryOrdering::Acquire);
    }
    break;
  }
  case AARCH64_INS_LDAPUR:
  case AARCH64_INS_LDAPURB:
  case AARCH64_INS_LDAPURH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = orderedAccessWidth(Insn, Dst.Size);
    if (Asz < Dst.Size) {
      NdVar V = S.makeTemp(Asz);
      S.emit(NdOp::LOAD, V, {EA}, NdMemoryOrdering::Acquire);
      S.emit(NdOp::INT_ZEXT, Dst, {V});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA}, NdMemoryOrdering::Acquire);
    }
    break;
  }
  case AARCH64_INS_LDAPURSB:
  case AARCH64_INS_LDAPURSH:
  case AARCH64_INS_LDAPURSW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t LoadSz = 1;
    if (Insn->id == AARCH64_INS_LDAPURSH)
      LoadSz = 2;
    else if (Insn->id == AARCH64_INS_LDAPURSW)
      LoadSz = 4;
    NdVar Val = S.makeTemp(LoadSz);
    S.emit(NdOp::LOAD, Val, {EA}, NdMemoryOrdering::Acquire);
    S.emit(NdOp::INT_SEXT, Dst, {Val});
    break;
  }
  case AARCH64_INS_LDTR:
  case AARCH64_INS_LDTRB:
  case AARCH64_INS_LDTRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = orderedAccessWidth(Insn, Dst.Size);
    if (Asz < Dst.Size) {
      NdVar V = S.makeTemp(Asz);
      S.emit(NdOp::LOAD, V, {EA});
      S.emit(NdOp::INT_ZEXT, Dst, {V});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case AARCH64_INS_LDTRSB:
  case AARCH64_INS_LDTRSH:
  case AARCH64_INS_LDTRSW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t LoadSz = 1;
    if (Insn->id == AARCH64_INS_LDTRSH)
      LoadSz = 2;
    else if (Insn->id == AARCH64_INS_LDTRSW)
      LoadSz = 4;
    NdVar Val = S.makeTemp(LoadSz);
    S.emit(NdOp::LOAD, Val, {EA});
    S.emit(NdOp::INT_SEXT, Dst, {Val});
    break;
  }
  case AARCH64_INS_LDPSW: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst1 = L.operandWrite(ARM64.operands[0]);
    NdVar Dst2 = L.operandWrite(ARM64.operands[1]);

    // Compute the effective address from base + disp; never operandRead the
    // memory operand (that would load the value and deref it as a pointer).
    auto &M = ARM64.operands[2];
    NdVar EA = S.makeTemp(8);
    if (M.type == AARCH64_OP_MEM) {
      bool First = true;
      auto Acc = [&](NdVar V) {
        if (First) {
          S.emit(NdOp::COPY, EA, {V});
          First = false;
        } else
          S.emit(NdOp::INT_ADD, EA, {EA, V});
      };
      if (M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        Acc(NdVar::reg(RI.Offset, 8));
      }
      // Post-index: displacement is the write-back amount, not part of the EA.
      if (M.mem.disp != 0 && !ARM64.post_index)
        Acc(NdVar::cst(static_cast<uint64_t>(M.mem.disp), 8));
      if (First)
        Acc(NdVar::cst(0, 8));
    } else {
      S.emit(NdOp::COPY, EA, {L.operandEffAddr(S, M)});
    }

    NdVar V1 = S.makeTemp(4);
    S.emit(NdOp::LOAD, V1, {EA});
    S.emit(NdOp::INT_SEXT, Dst1, {V1});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 8)});
    NdVar V2 = S.makeTemp(4);
    S.emit(NdOp::LOAD, V2, {EA2});
    S.emit(NdOp::INT_SEXT, Dst2, {V2});

    if (M.type == AARCH64_OP_MEM && Insn->detail->writeback &&
        M.mem.base != AARCH64_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
      NdVar BaseReg = NdVar::reg(RI.Offset, 8);
      if (ARM64.post_index) {
        int64_t WBOffset = M.mem.disp;
        if (ARM64.op_count >= 4 && ARM64.operands[3].type == AARCH64_OP_IMM)
          WBOffset = ARM64.operands[3].imm;
        if (WBOffset != 0)
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(WBOffset), 8)});
      } else {
        S.emit(NdOp::COPY, BaseReg, {EA});
      }
    }
    break;
  }
  case AARCH64_INS_LDRAA:
  case AARCH64_INS_LDRAB: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    S.emit(NdOp::LOAD, Dst, {EA});
    break;
  }
  case AARCH64_INS_LDIAPP: {
    if (ARM64.op_count < 3)
      break;
    NdVar LowDst = L.operandWrite(ARM64.operands[0]);
    NdVar HighDst = L.operandWrite(ARM64.operands[1]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[2]);
    NdVar Pair = S.makeTemp(LowDst.Size + HighDst.Size);
    S.emit(NdOp::LOAD, Pair, {EA}, NdMemoryOrdering::Acquire);
    S.emit(NdOp::SUBBYTES, LowDst, {Pair, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, HighDst, {Pair, NdVar::cst(LowDst.Size, 4)});
    break;
  }
  case AARCH64_INS_LDAP1: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
    S.emit(NdOp::LOAD, Dst, {EA});
    break;
  }
  case AARCH64_INS_LD64B: {
    if (ARM64.op_count < 2)
      break;
    NdVar FirstDst = L.operandWrite(ARM64.operands[0]);
    if (!FirstDst.isReg() || FirstDst.Size != 8)
      break;
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    for (unsigned I = 0; I < 8; ++I) {
      NdVar WordEA = EA;
      if (I != 0) {
        WordEA = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, WordEA,
               {EA, NdVar::cst(static_cast<uint64_t>(I * 8), 8)});
      }
      NdVar Dst = NdVar::reg(FirstDst.Offset + I * 8, 8);
      S.emit(NdOp::LOAD, Dst, {WordEA});
    }
    break;
  }

  // ========================================================================
  // Store variants (STLLR, STLUR, STTR, ST64B, STILP, STL1, etc.)
  // ========================================================================
  case AARCH64_INS_STLLR:
  case AARCH64_INS_STLLRB:
  case AARCH64_INS_STLLRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = orderedAccessWidth(Insn, Src.Size);
    if (Asz < Src.Size) {
      NdVar T = S.makeTemp(Asz);
      S.emit(NdOp::SUBBYTES, T, {Src, NdVar::cst(0, 4)});
      Src = T;
    }
    S.emit(NdOp::STORE, {}, {EA, Src}, NdMemoryOrdering::Release);
    break;
  }
  case AARCH64_INS_STLUR:
  case AARCH64_INS_STLURB:
  case AARCH64_INS_STLURH: {
    NdVar Src;
    NdVar EA;
    if (Insn->size == 4) {
      Src = storeSourceFromEncoding(Insn);
      EA = storeEffAddrFromEncoding(S, Insn);
    } else if (ARM64.op_count >= 2) {
      Src = L.operandRead(S, ARM64.operands[0]);
      EA = L.operandEffAddr(S, ARM64.operands[1]);
    } else {
      break;
    }
    uint16_t Asz = orderedAccessWidth(Insn, Src.Size);
    if (Asz < Src.Size) {
      NdVar T = S.makeTemp(Asz);
      S.emit(NdOp::SUBBYTES, T, {Src, NdVar::cst(0, 4)});
      Src = T;
    }
    S.emit(NdOp::STORE, {}, {EA, Src}, NdMemoryOrdering::Release);
    break;
  }
  case AARCH64_INS_STTR:
  case AARCH64_INS_STTRB:
  case AARCH64_INS_STTRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = orderedAccessWidth(Insn, Src.Size);
    if (Asz < Src.Size) {
      NdVar T = S.makeTemp(Asz);
      S.emit(NdOp::SUBBYTES, T, {Src, NdVar::cst(0, 4)});
      Src = T;
    }
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_ST64B:
  case AARCH64_INS_ST64BV:
  case AARCH64_INS_ST64BV0: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_STILP:
  case AARCH64_INS_STL1: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_SWPP:
  case AARCH64_INS_SWPPA:
  case AARCH64_INS_SWPPAL:
  case AARCH64_INS_SWPPL: {
    if (ARM64.op_count < 3)
      break;
    NdVar LowSrc = L.operandRead(S, ARM64.operands[0]);
    NdVar HighSrc = L.operandRead(S, ARM64.operands[1]);
    NdVar LowDst = L.operandWrite(ARM64.operands[0]);
    NdVar HighDst = L.operandWrite(ARM64.operands[1]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[2]);
    NdVar NewPair = S.makeTemp(LowSrc.Size + HighSrc.Size);
    NdVar OldPair = S.makeTemp(NewPair.Size);
    S.emit(NdOp::CONCAT, NewPair, {HighSrc, LowSrc});

    NdMemoryOrdering Ordering = NdMemoryOrdering::Relaxed;
    if (Insn->id == AARCH64_INS_SWPPA)
      Ordering = NdMemoryOrdering::Acquire;
    else if (Insn->id == AARCH64_INS_SWPPL)
      Ordering = NdMemoryOrdering::Release;
    else if (Insn->id == AARCH64_INS_SWPPAL)
      Ordering = NdMemoryOrdering::AcquireRelease;

    S.emit(NdOp::ATOMIC_XCHG, OldPair, {EA, NewPair}, Ordering);
    S.emit(NdOp::SUBBYTES, LowDst, {OldPair, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, HighDst,
           {OldPair, NdVar::cst(LowDst.Size, 4)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
