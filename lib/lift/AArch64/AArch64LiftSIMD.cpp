//===- AArch64LiftSIMD.cpp - AArch64 SIMD/atomic instruction lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SIMD, atomic, RCW, and Extended instruction handlers for AArch64.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-aarch64"

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
  Rt = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RtIdx
                                     : AARCH64_REG_W0 + RtIdx);
  Rn = static_cast<aarch64_reg>(AARCH64_REG_X0 + RnIdx);
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
    S.emit(NdOp::INT_ADD, EA,
           {EA, NdVar::cst(static_cast<uint64_t>(Imm9), 8)});
  return EA;
}

bool AArch64Lifter::liftSIMD(LiftState &S, const cs_insn *Insn,
                             const cs_aarch64 &ARM64) {
  switch (Insn->id) {

  // ========================================================================
  // Crypto — AES / SHA / SM3 / SM4.
  // ========================================================================
  // AESE/AESD are destructive single-round ops: Vd = op(Vd, Vn) — they consume
  // the old Vd (state) plus the key Vn.  AESMC/AESIMC mix only the source.
  case AARCH64_INS_AESE:
  case AARCH64_INS_AESD: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar Key = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Insn->id == AARCH64_INS_AESE ? Intrinsic::A64_Aese
                                                 : Intrinsic::A64_Aesd,
                    Dst, {OldD, Key});
    break;
  }
  case AARCH64_INS_AESMC:
  case AARCH64_INS_AESIMC: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Insn->id == AARCH64_INS_AESMC ? Intrinsic::A64_Aesmc
                                                  : Intrinsic::A64_Aesimc,
                    Dst, {Src});
    break;
  }

  // SHA1H reads a single scalar source; SHA1SU1/SHA256SU0 are destructive with
  // one extra source; the remaining round/schedule ops are destructive with two
  // extra sources (SHA1C/P/M take a scalar hash_e as the first extra source).
  case AARCH64_INS_SHA1H: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sha1h, Dst, {Src});
    break;
  }
  case AARCH64_INS_SHA1SU1:
  case AARCH64_INS_SHA256SU0: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Insn->id == AARCH64_INS_SHA1SU1 ? Intrinsic::A64_Sha1su1
                                                    : Intrinsic::A64_Sha256su0,
                    Dst, {OldD, Src});
    break;
  }
  case AARCH64_INS_SHA1C:
  case AARCH64_INS_SHA1P:
  case AARCH64_INS_SHA1M:
  case AARCH64_INS_SHA1SU0:
  case AARCH64_INS_SHA256H:
  case AARCH64_INS_SHA256H2:
  case AARCH64_INS_SHA256SU1: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case AARCH64_INS_SHA1C:
      Id = Intrinsic::A64_Sha1c;
      break;
    case AARCH64_INS_SHA1P:
      Id = Intrinsic::A64_Sha1p;
      break;
    case AARCH64_INS_SHA1M:
      Id = Intrinsic::A64_Sha1m;
      break;
    case AARCH64_INS_SHA1SU0:
      Id = Intrinsic::A64_Sha1su0;
      break;
    case AARCH64_INS_SHA256H:
      Id = Intrinsic::A64_Sha256h;
      break;
    case AARCH64_INS_SHA256H2:
      Id = Intrinsic::A64_Sha256h2;
      break;
    default:
      Id = Intrinsic::A64_Sha256su1;
      break;
    }
    S.emitIntrinsic(Id, Dst, {OldD, A, B});
    break;
  }

  // SHA512 (FEAT_SHA512, ARMv8.2): destructive 128-bit (2 x i64) hash ops.  Map
  // to the LLVM crypto intrinsic so codegen emits the real sha512h/h2/su0/su1
  // (Unicorn's MAX CPU executes it bit-exactly).  Was a ShaGeneric placeholder
  // that only copied one source, folding the result to garbage.
  case AARCH64_INS_SHA512H:
  case AARCH64_INS_SHA512H2:
  case AARCH64_INS_SHA512SU1: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    Intrinsic Id = Insn->id == AARCH64_INS_SHA512H ? Intrinsic::A64_Sha512h
                   : Insn->id == AARCH64_INS_SHA512H2
                       ? Intrinsic::A64_Sha512h2
                       : Intrinsic::A64_Sha512su1;
    S.emitIntrinsic(Id, Dst, {OldD, A, B});
    break;
  }
  case AARCH64_INS_SHA512SU0: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sha512su0, Dst, {OldD, A});
    break;
  }

  // SM3 (FEAT_SM3, ARMv8.2) message expansion (PARTW1/PARTW2, destructive) and
  // compression (SS1 four-register non-destructive; TT1A/1B/2A/2B destructive
  // with a 2-bit lane index on Vm).  Were an AesGeneric placeholder.
  case AARCH64_INS_SM3PARTW1:
  case AARCH64_INS_SM3PARTW2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Insn->id == AARCH64_INS_SM3PARTW1
                        ? Intrinsic::A64_Sm3partw1
                        : Intrinsic::A64_Sm3partw2,
                    Dst, {OldD, A, B});
    break;
  }
  case AARCH64_INS_SM3SS1: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar C = operandRead(S, ARM64.operands[3]);
    S.emitIntrinsic(Intrinsic::A64_Sm3ss1, Dst, {A, B, C});
    break;
  }
  case AARCH64_INS_SM3TT1A:
  case AARCH64_INS_SM3TT1B:
  case AARCH64_INS_SM3TT2A:
  case AARCH64_INS_SM3TT2B: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    // operands[2] is `Vm.s[index]`; reading it directly would extract just that
    // one lane (operandRead's vector_index path).  The intrinsic needs the FULL
    // Vm vector plus the lane index as a separate ImmArg, so clear the index
    // when reading and pass it through explicitly.
    cs_aarch64_op VmOp = ARM64.operands[2];
    int Idx = VmOp.vector_index;
    VmOp.vector_index = -1;
    NdVar B = operandRead(S, VmOp);
    Intrinsic Id = Insn->id == AARCH64_INS_SM3TT1A   ? Intrinsic::A64_Sm3tt1a
                   : Insn->id == AARCH64_INS_SM3TT1B ? Intrinsic::A64_Sm3tt1b
                   : Insn->id == AARCH64_INS_SM3TT2A ? Intrinsic::A64_Sm3tt2a
                                                     : Intrinsic::A64_Sm3tt2b;
    S.emitIntrinsic(
        Id, Dst, {OldD, A, B, NdVar::cst(Idx >= 0 ? (uint64_t)Idx : 0, 8)});
    break;
  }

  // SM4 (FEAT_SM4, ARMv8.2): SM4E is destructive (Vd ^= round(Vn)); SM4EKEY is
  // non-destructive key expansion (Vd = f(Vn, Vm)).  Were AesGeneric
  // placeholders.
  case AARCH64_INS_SM4E: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sm4e, Dst, {OldD, A});
    break;
  }
  case AARCH64_INS_SM4EKEY: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Intrinsic::A64_Sm4ekey, Dst, {A, B});
    break;
  }

  // BCAX — bit clear and xor. Dst = a ^ (b & ~C).
  case AARCH64_INS_BCAX: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar C = operandRead(S, ARM64.operands[3]);
    NdVar NC = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NC, {C});
    NdVar BC = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, BC, {B, NC});
    S.emit(NdOp::INT_XOR, Dst, {A, BC});
    break;
  }

  // ========================================================================
  // Float additional: FMULX, FNMUL, dot products.
  // ========================================================================
  case AARCH64_INS_FMULX: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    // FMULX is FP multiply-extended: like FMUL for finite operands but
    // 0*Inf = +/-2.0 (so reciprocal Newton-Raphson steps stay defined at the
    // boundaries).  Plain FLOAT_MULT both loses that special case (-> NaN) and,
    // for vectors, collapses the whole register into one f32/f64 (the emitter
    // truncates a 128-bit operand to its low 64 bits), corrupting every lane.
    // Map to the real aarch64.neon.fmulx intrinsic; the float element width
    // (f32/f64) is passed as a trailing constant, mirroring FRECPS/FRSQRTS.
    unsigned ElemSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      ElemSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      ElemSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      ElemSz = 2; // half-precision (FEAT_FP16) vectors
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2)   ? 2
               : (Dst.Size >= 8) ? 8
                                 : 4; // scalar H/S/D fall back to dst width
    S.emitIntrinsic(Intrinsic::A64_Fmulx, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }

  case AARCH64_INS_FNMUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_NEG, Dst, {Prod});
    break;
  }

  // SDOT / UDOT — dot product accumulate.  Each 32-bit destination lane i adds
  // the sum of four byte products: Dst[i] += sum_{k=0..3}
  // ext(A[4i+k])*ext(B[..]). The indexed form (`udot v.4s, v.16b, vN.4b[idx]`)
  // broadcasts one 32-bit (4-byte) group of B selected by vector_index.  The
  // old code was a full-width INT_MULT+INT_ADD placeholder (whole register as
  // one integer — completely wrong, no per-lane reduction, no byte widening).
  case AARCH64_INS_SDOT:
  case AARCH64_INS_UDOT: {
    if (ARM64.op_count < 3)
      break;
    bool IsSigned = (Insn->id == AARCH64_INS_SDOT);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    unsigned NLanes = Dst.Size / 4;
    int VecIdx = ARM64.operands[2].vector_index;
    auto ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    NdVar Acc = NdVar::cst(0, 0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned BBase = (VecIdx >= 0 ? (unsigned)VecIdx : I) * 4;
      NdVar Lane = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Lane, {OldDst, NdVar::cst(I * 4, 4)});
      for (unsigned K = 0; K < 4; ++K) {
        NdVar Ba = S.makeTemp(1), Bb = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ba, {A, NdVar::cst(I * 4 + K, 4)});
        S.emit(NdOp::SUBBYTES, Bb, {B, NdVar::cst(BBase + K, 4)});
        NdVar Ea = S.makeTemp(4), Eb = S.makeTemp(4);
        S.emit(ExtOp, Ea, {Ba});
        S.emit(ExtOp, Eb, {Bb});
        NdVar Pr = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Pr, {Ea, Eb});
        NdVar Nl = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Nl, {Lane, Pr});
        Lane = Nl;
      }
      if (I == 0) {
        Acc = Lane;
      } else {
        NdVar P = S.makeTemp(Acc.Size + 4);
        S.emit(NdOp::CONCAT, P, {Lane, Acc});
        Acc = P;
      }
    }
    if (Acc.Size < Dst.Size)
      S.emit(NdOp::INT_ZEXT, Dst, {Acc});
    else
      S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // ========================================================================
  // Flag manipulation: CFINV, XAFLAG, RMIF, SETF.
  // ========================================================================
  case AARCH64_INS_CFINV: {
    NdVar CF = NdVar::reg(a64reg::CFLAG, 1);
    S.emit(NdOp::BOOL_NOT, CF, {CF});
    break;
  }
  case AARCH64_INS_XAFLAG: {
    // FEAT_FlagM2: convert NZCV from the "alternative" (JavaScript) FP-compare
    // encoding back to the Arm encoding.  Depends only on C and Z (per the
    // ARM ARM / QEMU `gen_xaflag`):
    //   N = NOT C AND NOT Z;  Z = C AND Z;  C = C OR Z;  V = NOT C AND Z.
    // Was a bare opaque `A64_Xaflag` intrinsic that left NeverD's modelled
    // flags (NFLAG/ZFLAG/CFLAG/VFLAG) untouched, so the conversion was lost.
    NdVar OldZ = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZ, {NdVar::reg(a64reg::ZFLAG, 1)});
    NdVar OldC = S.makeTemp(1);
    S.emit(NdOp::COPY, OldC, {NdVar::reg(a64reg::CFLAG, 1)});
    NdVar NotC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotC, {OldC});
    NdVar NotZ = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotZ, {OldZ});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::NFLAG, 1), {NotC, NotZ});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::ZFLAG, 1), {OldC, OldZ});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::VFLAG, 1), {NotC, OldZ});
    S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::CFLAG, 1), {OldC, OldZ});
    break;
  }
  case AARCH64_INS_RMIF: {
    // FEAT_FlagM: rotate Xn right by #shift, then insert the low 4 bits of the
    // rotated value into NZCV under the 4-bit #mask — bit3->N, bit2->Z,
    // bit1->C, bit0->V — leaving flags not selected by the mask unchanged.
    // Was a bare opaque `A64_Rmif` intrinsic that dropped all three operands
    // and set no flags at all (the rotate, the mask, and the insert were lost).
    if (ARM64.op_count < 3)
      break;
    NdVar Xn = operandRead(S, ARM64.operands[0]);
    uint64_t Shift = static_cast<uint64_t>(ARM64.operands[1].imm) & 63;
    uint64_t Mask = static_cast<uint64_t>(ARM64.operands[2].imm) & 0xF;
    // tmp = ROR(Xn, Shift) over the full 64-bit register.
    NdVar Tmp = Xn;
    if (Shift != 0) {
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, Lo, {Xn, NdVar::cst(Shift, 8)});
      NdVar Hi = S.makeTemp(8);
      S.emit(NdOp::INT_LEFT, Hi, {Xn, NdVar::cst(64 - Shift, 8)});
      Tmp = S.makeTemp(8);
      S.emit(NdOp::INT_OR, Tmp, {Lo, Hi});
    }
    const struct {
      unsigned Bit;
      uint64_t Reg;
    } FlagMap[4] = {{3, a64reg::NFLAG},
                    {2, a64reg::ZFLAG},
                    {1, a64reg::CFLAG},
                    {0, a64reg::VFLAG}};
    for (const auto &FM : FlagMap) {
      if (!(Mask & (1u << FM.Bit)))
        continue;
      NdVar Sh = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, Sh, {Tmp, NdVar::cst(FM.Bit, 8)});
      NdVar Bit = S.makeTemp(8);
      S.emit(NdOp::INT_AND, Bit, {Sh, NdVar::cst(1, 8)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(FM.Reg, 1),
             {Bit, NdVar::cst(0, 8)});
    }
    break;
  }
  case AARCH64_INS_SETF8:
  case AARCH64_INS_SETF16: {
    // "Evaluate into flags": set NZV as if an 8-bit (SETF8) or 16-bit (SETF16)
    // signed value had been produced, leaving C unchanged.  Per the ARM ARM /
    // QEMU `disas_evaluate_into_flags` (msb = 7 for SETF8, 15 for SETF16):
    //   N = operand<msb>;  Z = (operand<msb:0> == 0);
    //   V = operand<msb+1> EOR operand<msb>;  C unchanged.
    // The old code took N from bit 31 and Z from the whole 32-bit value and
    // never wrote V, so every byte/halfword flag came out wrong (small inputs
    // masked it: low byte == full word, bit7 == bit31 == 0).
    if (ARM64.op_count < 1)
      break;
    NdVar Src = operandRead(S, ARM64.operands[0]);
    bool Is8 = (Insn->id == AARCH64_INS_SETF8);
    unsigned MsbBit = Is8 ? 7 : 15;
    uint16_t NarrowSz = Is8 ? 1 : 2;
    // N and Z come from the low byte / halfword only.
    NdVar Narrow = S.makeTemp(NarrowSz);
    S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
           {Narrow, NdVar::cst(0, NarrowSz)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
           {Narrow, NdVar::cst(0, NarrowSz)});
    // V = bit[msb+1] XOR bit[msb] (signed overflow out of the msb).
    NdVar Sh0 = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_RIGHT, Sh0, {Src, NdVar::cst(MsbBit, Src.Size)});
    NdVar Sh1 = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_RIGHT, Sh1, {Src, NdVar::cst(MsbBit + 1, Src.Size)});
    NdVar Xr = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_XOR, Xr, {Sh0, Sh1});
    NdVar VBit = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_AND, VBit, {Xr, NdVar::cst(1, Src.Size)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(a64reg::VFLAG, 1),
           {VBit, NdVar::cst(0, Src.Size)});
    break;
  }

  // MTE: ADDG / SUBG
  case AARCH64_INS_ADDG:
  case AARCH64_INS_SUBG: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(Insn->id == AARCH64_INS_ADDG ? NdOp::INT_ADD : NdOp::INT_SUB, Dst,
           {Src, NdVar::cst(0x10, Src.Size)});
    break;
  }

  // AUTDA / AUTDZA / AUTDZA / AUTDB / AUTDZB — pointer authentication (data).
  case AARCH64_INS_AUTDA:
  case AARCH64_INS_AUTDZA:
  case AARCH64_INS_AUTDB:
  case AARCH64_INS_AUTDZB: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Insn->id == AARCH64_INS_AUTDA ||
                              Insn->id == AARCH64_INS_AUTDZA
                          ? Intrinsic::Autda
                          : Intrinsic::Autdb,
                      Dst);
    }
    break;
  }

  // PACIA/PACIB/PACDA/PACDB — additional PAC variants.
  case AARCH64_INS_PACDA:
  case AARCH64_INS_PACDZA:
  case AARCH64_INS_PACDB:
  case AARCH64_INS_PACDZB: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Insn->id == AARCH64_INS_PACDA ||
                              Insn->id == AARCH64_INS_PACDZA
                          ? Intrinsic::Pacda
                          : Intrinsic::Pacdb,
                      Dst);
    }
    break;
  }

  // XPACD — strip PAC from data pointer.
  case AARCH64_INS_XPACD: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Intrinsic::Xpacd, Dst);
    }
    break;
  }

  // SXTB / SXTH / UXTB / UXTH — extract sub-byte/halfword then extend.
  // Capstone reports the source as a w-register (32-bit), but the instruction
  // only uses bits [7:0] (SXTB/UXTB) or [15:0] (SXTH/UXTH).
  case AARCH64_INS_SXTB:
  case AARCH64_INS_SXTH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    uint16_t ExtSz = (Insn->id == AARCH64_INS_SXTB) ? 1 : 2;
    NdVar Narrow = S.makeTemp(ExtSz);
    S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Narrow});
    break;
  }
  case AARCH64_INS_UXTB:
  case AARCH64_INS_UXTH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    uint16_t ExtSz = (Insn->id == AARCH64_INS_UXTB) ? 1 : 2;
    NdVar Narrow = S.makeTemp(ExtSz);
    S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_ZEXT, Dst, {Narrow});
    break;
  }

  // WFE/WFI/YIELD/SEV/SEVL/NOP are encoded as HINT in Capstone 6,
  // already handled at the top of the switch.
  // DC/IC/TLBI/AT are encoded as SYS, already handled above.

  // ========================================================================
  // CRC32 (ARMv8.0-CRC)
  // ========================================================================
  case AARCH64_INS_CRC32B:
  case AARCH64_INS_CRC32H:
  case AARCH64_INS_CRC32W:
  case AARCH64_INS_CRC32X:
  case AARCH64_INS_CRC32CB:
  case AARCH64_INS_CRC32CH:
  case AARCH64_INS_CRC32CW:
  case AARCH64_INS_CRC32CX: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case AARCH64_INS_CRC32B:
      Id = Intrinsic::A64_Crc32b;
      break;
    case AARCH64_INS_CRC32H:
      Id = Intrinsic::A64_Crc32h;
      break;
    case AARCH64_INS_CRC32W:
      Id = Intrinsic::A64_Crc32w;
      break;
    case AARCH64_INS_CRC32X:
      Id = Intrinsic::A64_Crc32x;
      break;
    case AARCH64_INS_CRC32CB:
      Id = Intrinsic::A64_Crc32cb;
      break;
    case AARCH64_INS_CRC32CH:
      Id = Intrinsic::A64_Crc32ch;
      break;
    case AARCH64_INS_CRC32CW:
      Id = Intrinsic::A64_Crc32cw;
      break;
    case AARCH64_INS_CRC32CX:
      Id = Intrinsic::A64_Crc32cx;
      break;
    default:
      Id = Intrinsic::A64_Crc32b;
      break;
    }
    S.emitIntrinsic(Id, Dst, {A, B});
    break;
  }

  // ========================================================================
  // PAC Extended variants (ARMv8.3+)
  // ========================================================================
  case AARCH64_INS_BLRAA:
  case AARCH64_INS_BLRAB: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::COPY, NdVar::reg(a64reg::X30, 8),
           {NdVar::cst(S.Addr + 4, 8)});
    S.emit(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8), {Target});
    break;
  }
  case AARCH64_INS_BLRAAZ:
  case AARCH64_INS_BLRABZ: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::COPY, NdVar::reg(a64reg::X30, 8),
           {NdVar::cst(S.Addr + 4, 8)});
    S.emit(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8), {Target});
    break;
  }
  case AARCH64_INS_BRAA:
  case AARCH64_INS_BRAB: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }
  case AARCH64_INS_BRAAZ:
  case AARCH64_INS_BRABZ: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }
  case AARCH64_INS_RETAA:
  case AARCH64_INS_RETAB:
  case AARCH64_INS_RETAASPPC:
  case AARCH64_INS_RETABSPPC:
    S.emit(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)});
    break;
  case AARCH64_INS_ERETAA:
  case AARCH64_INS_ERETAB:
    S.emitIntrinsic(Intrinsic::Eret);
    break;
  case AARCH64_INS_AUTIA171615:
  case AARCH64_INS_AUTIASPPC:
    S.emitIntrinsic(Intrinsic::Autia);
    break;
  case AARCH64_INS_AUTIB171615:
  case AARCH64_INS_AUTIBSPPC:
    S.emitIntrinsic(Intrinsic::Autib);
    break;
  case AARCH64_INS_PACIA171615:
  case AARCH64_INS_PACIASPPC:
  case AARCH64_INS_PACNBIASPPC:
    S.emitIntrinsic(Intrinsic::Pacia);
    break;
  case AARCH64_INS_PACIB171615:
  case AARCH64_INS_PACIBSPPC:
  case AARCH64_INS_PACNBIBSPPC:
    S.emitIntrinsic(Intrinsic::Pacib);
    break;
  case AARCH64_INS_PACGA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
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
  case AARCH64_INS_LDAPUR:
  case AARCH64_INS_LDAPURB:
  case AARCH64_INS_LDAPURH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
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
  case AARCH64_INS_LDAPURSB:
  case AARCH64_INS_LDAPURSH:
  case AARCH64_INS_LDAPURSW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    uint16_t LoadSz = 1;
    if (Insn->id == AARCH64_INS_LDAPURSH)
      LoadSz = 2;
    else if (Insn->id == AARCH64_INS_LDAPURSW)
      LoadSz = 4;
    NdVar Val = S.makeTemp(LoadSz);
    S.emit(NdOp::LOAD, Val, {EA});
    S.emit(NdOp::INT_SEXT, Dst, {Val});
    break;
  }
  case AARCH64_INS_LDTR:
  case AARCH64_INS_LDTRB:
  case AARCH64_INS_LDTRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
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
    NdVar Dst1 = operandWrite(ARM64.operands[0]);
    NdVar Dst2 = operandWrite(ARM64.operands[1]);

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
      S.emit(NdOp::COPY, EA, {operandEffAddr(S, M)});
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    S.emit(NdOp::LOAD, Dst, {EA});
    break;
  }
  case AARCH64_INS_LDIAPP:
  case AARCH64_INS_LDAP1: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
    S.emit(NdOp::LOAD, Dst, {EA});
    break;
  }
  case AARCH64_INS_LD64B: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    S.emit(NdOp::LOAD, Dst, {EA});
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
    NdVar Src = operandRead(S, ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = orderedAccessWidth(Insn, Src.Size);
    if (Asz < Src.Size) {
      NdVar T = S.makeTemp(Asz);
      S.emit(NdOp::SUBBYTES, T, {Src, NdVar::cst(0, 4)});
      Src = T;
    }
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_STLUR:
  case AARCH64_INS_STLURB:
  case AARCH64_INS_STLURH: {
    NdVar Src;
    NdVar EA;
    if (ARM64.op_count >= 2) {
      Src = operandRead(S, ARM64.operands[0]);
      EA = operandEffAddr(S, ARM64.operands[1]);
    } else if (ARM64.op_count == 1 &&
               ARM64.operands[0].type == AARCH64_OP_MEM) {
      Src = storeSourceFromEncoding(Insn);
      EA = storeEffAddrFromEncoding(S, Insn);
    } else {
      break;
    }
    uint16_t Asz = orderedAccessWidth(Insn, Src.Size);
    if (Asz < Src.Size) {
      NdVar T = S.makeTemp(Asz);
      S.emit(NdOp::SUBBYTES, T, {Src, NdVar::cst(0, 4)});
      Src = T;
    }
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_STTR:
  case AARCH64_INS_STTRB:
  case AARCH64_INS_STTRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
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
    NdVar Src = operandRead(S, ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_STILP:
  case AARCH64_INS_STL1: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case AARCH64_INS_SWPP:
  case AARCH64_INS_SWPPA:
  case AARCH64_INS_SWPPAL:
  case AARCH64_INS_SWPPL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Src = operandRead(S, ARM64.operands[0]);
    NdVar Dst = operandWrite(ARM64.operands[1]);
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar OldVal = S.makeTemp(Dst.Size);
    S.emit(NdOp::LOAD, OldVal, {EA});
    S.emit(NdOp::COPY, Dst, {OldVal});
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }

  // ========================================================================
  // Memory tagging (MTE, ARMv8.5)
  // ========================================================================
  case AARCH64_INS_STG:
    S.emitIntrinsic(Intrinsic::Stg);
    break;
  case AARCH64_INS_STZG:
    S.emitIntrinsic(Intrinsic::Stzg);
    break;
  case AARCH64_INS_ST2G:
    S.emitIntrinsic(Intrinsic::St2g);
    break;
  case AARCH64_INS_STZ2G:
    S.emitIntrinsic(Intrinsic::Stz2g);
    break;
  case AARCH64_INS_STGM:
  case AARCH64_INS_STZGM: {
    S.emitIntrinsic(Intrinsic::Stg);
    break;
  }
  case AARCH64_INS_STGP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Src1 = operandRead(S, ARM64.operands[0]);
    NdVar Src2 = operandRead(S, ARM64.operands[1]);
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Src1.Size, 8)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    break;
  }
  case AARCH64_INS_LDG:
  case AARCH64_INS_LDGM: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {EA});
    break;
  }
  case AARCH64_INS_IRG:
  case AARCH64_INS_GMI: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_SUBP:
  case AARCH64_INS_SUBPS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    if (Insn->id == AARCH64_INS_SUBPS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
    }
    break;
  }

  // ========================================================================
  // Branch: BC (conditional branch with hint)
  // ========================================================================
  case AARCH64_INS_BC: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::BRANCH, {}, {Target});
    break;
  }

  // ========================================================================
  // Misc integer: UXTW, CTZ, AXFLAG, UDF
  // ========================================================================
  case AARCH64_INS_UXTW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_ZEXT, Dst, {Src});
    break;
  }
  case AARCH64_INS_CTZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    // CTZ counts TRAILING zeros; the old code emitted LZCOUNT (leading).
    // ctz(x) = popcount(~x & (x-1))  (mirrors the x86 TZCNT lowering).
    NdVar NotX = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_NOT, NotX, {Src});
    NdVar XM1 = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_SUB, XM1, {Src, NdVar::cst(1, Src.Size)});
    NdVar Iso = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_AND, Iso, {NotX, XM1});
    S.emit(NdOp::POPCOUNT, Dst, {Iso});
    break;
  }
  case AARCH64_INS_AXFLAG: {
    // FEAT_FlagM2: convert NZCV from the Arm encoding to the "alternative"
    // (JavaScript) FP-compare encoding.  Depends on Z, V, C (per the ARM ARM /
    // QEMU `gen_axflag`):
    //   N = 0;  Z = Z OR V;  C = C AND NOT V;  V = 0.
    // Was a bare opaque `A64_Axflag` intrinsic that left NeverD's modelled
    // flags untouched, so the conversion was lost.
    NdVar OldZ = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZ, {NdVar::reg(a64reg::ZFLAG, 1)});
    NdVar OldV = S.makeTemp(1);
    S.emit(NdOp::COPY, OldV, {NdVar::reg(a64reg::VFLAG, 1)});
    NdVar OldC = S.makeTemp(1);
    S.emit(NdOp::COPY, OldC, {NdVar::reg(a64reg::CFLAG, 1)});
    NdVar NotV = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotV, {OldV});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::NFLAG, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::ZFLAG, 1), {OldZ, OldV});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::CFLAG, 1), {OldC, NotV});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
    break;
  }
  case AARCH64_INS_UDF:
    S.emitIntrinsic(Intrinsic::Brk);
    break;

  // ========================================================================
  // System / debug / privileged
  // ========================================================================
  case AARCH64_INS_DCPS1:
  case AARCH64_INS_DCPS2:
  case AARCH64_INS_DCPS3:
  case AARCH64_INS_DRPS:
  case AARCH64_INS_SB:
  case AARCH64_INS_SDSB:
  case AARCH64_INS_TSB:
  case AARCH64_INS_TRCIT:
  case AARCH64_INS_BRB:
    S.emitIntrinsic(Intrinsic::A64_Brb);
    break;
  case AARCH64_INS_WFET:
    S.emitIntrinsic(Intrinsic::A64_Wfet);
    break;
  case AARCH64_INS_WFIT:
    S.emitIntrinsic(Intrinsic::A64_Wfit);
    break;
  case AARCH64_INS_SYSL: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Intrinsic::Dc, Dst);
    } else {
      S.emitIntrinsic(Intrinsic::Dc);
    }
    break;
  }
  case AARCH64_INS_SYSP:
    S.emitIntrinsic(Intrinsic::Dc);
    break;
  case AARCH64_INS_AT_AS1ELX:
    S.emitIntrinsic(Intrinsic::At);
    break;
  case AARCH64_INS_MRRS: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Intrinsic::Mrs, Dst);
    }
    break;
  }
  case AARCH64_INS_MSRR: {
    if (ARM64.op_count >= 1) {
      NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emitIntrinsic(Intrinsic::Msr, NdVar::reg(a64reg::X0, 8), {Src});
    }
    break;
  }

  // TME (Transactional Memory Extension)
  case AARCH64_INS_TSTART: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_TTEST: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_TCOMMIT:
    S.emitIntrinsic(Intrinsic::A64_Tcommit);
    break;
  case AARCH64_INS_TCANCEL:
    S.emitIntrinsic(Intrinsic::A64_Tcancel);
    break;

  // GCS (Guarded Control Stack)
  case AARCH64_INS_GCSPOPCX:
  case AARCH64_INS_GCSPOPM:
  case AARCH64_INS_GCSPOPX:
  case AARCH64_INS_GCSPUSHM:
  case AARCH64_INS_GCSPUSHX:
  case AARCH64_INS_GCSSS1:
  case AARCH64_INS_GCSSS2:
    S.emitIntrinsic(Intrinsic::A64_Gcsstr);
    break;
  case AARCH64_INS_GCSSTR:
    S.emitIntrinsic(Intrinsic::A64_Gcsstr);
    break;
  case AARCH64_INS_GCSSTTR:
    S.emitIntrinsic(Intrinsic::A64_Gcssttr);
    break;

  // GENTER / GEXIT (Guarded pages)
  case AARCH64_INS_GENTER:
    S.emitIntrinsic(Intrinsic::A64_Genter);
    break;
  case AARCH64_INS_GEXIT:
    S.emitIntrinsic(Intrinsic::A64_Gexit);
    break;

  // ========================================================================
  // NEON additional variants: RSHRN, SHRN, SABA, UABA, ABD long, etc.
  // ========================================================================
  // SHRN/SHRN2/RSHRN/RSHRN2 — shift right and NARROW: each wide source lane (Q
  // register, element 2*NarrowSz) is shifted right by #imm and truncated to its
  // low half.  SHRN writes the 64-bit narrowed result to the low half of the
  // dest (zeroing the high); the "2" variants write it to the high 64 bits,
  // preserving the low.  The old code emitted a single full-width INT_RIGHT,
  // shifting the whole 128-bit register and keeping only the low 64 bits —
  // correct for lane 0, wrong for the high lanes.  Narrow truncation makes the
  // shift sign-agnostic, so a logical shift is always correct.  (The saturating
  // SQSHRN/UQSHRN family below was already per-lane; plain SHRN/RSHRN was not.)
  case AARCH64_INS_RSHRN:
  case AARCH64_INS_RSHRN2:
  case AARCH64_INS_SHRN:
  case AARCH64_INS_SHRN2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned Imm = static_cast<unsigned>(ARM64.operands[2].imm);
    bool IsRound =
        (Insn->id == AARCH64_INS_RSHRN || Insn->id == AARCH64_INS_RSHRN2);
    bool Is2 =
        (Insn->id == AARCH64_INS_SHRN2 || Insn->id == AARCH64_INS_RSHRN2);
    unsigned NarrowSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      NarrowSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      NarrowSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      NarrowSz = 4;
      break;
    default:
      break;
    }
    if (NarrowSz == 0 || Src.Size != 16) {
      S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
      break;
    }
    unsigned WideSz = NarrowSz * 2;
    unsigned NLanes = 16 / WideSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar SLane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * WideSz, 4)});
      if (IsRound && Imm > 0) {
        NdVar Rounded = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Rounded,
               {SLane, NdVar::cst(1ull << (Imm - 1), WideSz)});
        SLane = Rounded;
      }
      NdVar Shifted = S.makeTemp(WideSz);
      S.emit(NdOp::INT_RIGHT, Shifted, {SLane, NdVar::cst(Imm, WideSz)});
      NdVar NLane = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, NLane, {Shifted, NdVar::cst(0, 4)});
      if (I == 0)
        Acc = NLane;
      else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {NLane, Acc});
        Acc = Next;
      }
    }
    if (Is2) {
      NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Lo, {OldDst, NdVar::cst(0, 4)});
      NdVar Full = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Full, {Acc, Lo});
      S.emit(NdOp::COPY, Dst, {Full});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // Saturating / rounding variable shift (register form, per-lane signed
  // amount).  Map to the AArch64 NEON intrinsic; the old code was a plain
  // full-width INT_LEFT (no saturation/rounding, no per-lane, no right shift).
  case AARCH64_INS_SQRSHL:
  case AARCH64_INS_UQRSHL:
  case AARCH64_INS_SRSHL:
  case AARCH64_INS_URSHL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      ElemSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      ElemSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      ElemSz = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
    case AARCH64LAYOUT_VL_1D:
      ElemSz = 8;
      break;
    default:
      break;
    }
    if (ElemSz == 0)
      ElemSz = Dst.Size;
    Intrinsic II = (Insn->id == AARCH64_INS_SQRSHL)   ? Intrinsic::A64_Sqrshl
                   : (Insn->id == AARCH64_INS_UQRSHL) ? Intrinsic::A64_Uqrshl
                   : (Insn->id == AARCH64_INS_SRSHL)  ? Intrinsic::A64_Srshl
                                                      : Intrinsic::A64_Urshl;
    S.emitIntrinsic(II, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  // Narrowing saturating shift-right by immediate.  Map to the AArch64 NEON
  // intrinsic (wide vector + i32 imm -> narrow vector); the "2" variants write
  // the narrowed result into the high 64 bits, preserving the low 64.  The old
  // code was a plain full-width INT_RIGHT (no narrowing/saturation/per-lane).
  case AARCH64_INS_SQRSHRN:
  case AARCH64_INS_SQRSHRN2:
  case AARCH64_INS_SQRSHRUN:
  case AARCH64_INS_SQRSHRUN2:
  case AARCH64_INS_SQSHRN:
  case AARCH64_INS_SQSHRN2:
  case AARCH64_INS_SQSHRUN:
  case AARCH64_INS_SQSHRUN2:
  case AARCH64_INS_UQRSHRN:
  case AARCH64_INS_UQRSHRN2:
  case AARCH64_INS_UQSHRN:
  case AARCH64_INS_UQSHRN2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned Imm = static_cast<unsigned>(ARM64.operands[2].imm);
    unsigned NarrowSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      NarrowSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      NarrowSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      NarrowSz = 4;
      break;
    default:
      break;
    }
    bool Is2 =
        (Insn->id == AARCH64_INS_SQRSHRN2 ||
         Insn->id == AARCH64_INS_SQRSHRUN2 || Insn->id == AARCH64_INS_SQSHRN2 ||
         Insn->id == AARCH64_INS_SQSHRUN2 || Insn->id == AARCH64_INS_UQRSHRN2 ||
         Insn->id == AARCH64_INS_UQSHRN2);
    if (NarrowSz == 0 || Src.Size != 16) {
      S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
      break;
    }
    Intrinsic II;
    switch (Insn->id) {
    case AARCH64_INS_SQSHRN:
    case AARCH64_INS_SQSHRN2:
      II = Intrinsic::A64_Sqshrn;
      break;
    case AARCH64_INS_SQRSHRN:
    case AARCH64_INS_SQRSHRN2:
      II = Intrinsic::A64_Sqrshrn;
      break;
    case AARCH64_INS_UQSHRN:
    case AARCH64_INS_UQSHRN2:
      II = Intrinsic::A64_Uqshrn;
      break;
    case AARCH64_INS_SQSHRUN:
    case AARCH64_INS_SQSHRUN2:
      II = Intrinsic::A64_Sqshrun;
      break;
    case AARCH64_INS_SQRSHRUN:
    case AARCH64_INS_SQRSHRUN2:
      II = Intrinsic::A64_Sqrshrun;
      break;
    default:
      II = Intrinsic::A64_Uqrshrn;
      break;
    }
    NdVar Narrow = S.makeTemp((16u / (NarrowSz * 2)) * NarrowSz);
    S.emitIntrinsic(II, Narrow,
                    {Src, NdVar::cst(Imm, 4), NdVar::cst(NarrowSz, 4)});
    if (Is2) {
      NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Lo, {OldDst, NdVar::cst(0, 4)});
      NdVar Full = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Full, {Narrow, Lo});
      S.emit(NdOp::COPY, Dst, {Full});
    } else {
      S.emit(NdOp::COPY, Dst, {Narrow});
    }
    break;
  }
  case AARCH64_INS_SABA:
  case AARCH64_INS_UABA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar OldD = operandRead(S, ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SABA);
    unsigned LaneSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      break;
    default:
      break;
    }
    // dst[i] += |a[i] - b[i]| per lane.  A full-width INT_SUB would let the
    // borrow cross lanes and drops the absolute value entirely.
    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldD, NdVar::cst(I * LaneSz, 4)});
        NdVar Diff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_SUB, Diff, {La, Lb});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {La, Lb});
        NdVar NegDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
        NdVar AbsDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
        NdVar LaneRes = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ADD, LaneRes, {Ld, AbsDiff});
        if (I == 0) {
          Acc = LaneRes;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Diff = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Diff, {A, B});
      NdVar IsNeg = S.makeTemp(1);
      S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {A, B});
      NdVar NegDiff = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
      NdVar AbsDiff = S.makeTemp(Dst.Size);
      S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
      S.emit(NdOp::INT_ADD, Dst, {OldD, AbsDiff});
    }
    break;
  }
  case AARCH64_INS_SABDL:
  case AARCH64_INS_SABDL2:
  case AARCH64_INS_UABDL:
  case AARCH64_INS_UABDL2:
  case AARCH64_INS_SABAL:
  case AARCH64_INS_SABAL2:
  case AARCH64_INS_UABAL:
  case AARCH64_INS_UABAL2: {
    // Widening absolute difference (and accumulate): Dst[i](wide) =
    // |widen(A[i]) - widen(B[i])| (+ old Dst[i] for the AL variants).  A plain
    // full-width INT_SUB drops the per-lane absolute value and lets borrows
    // cross lanes; the "2" variants take the upper 64 bits of the narrow
    // sources.
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SABDL || Insn->id == AARCH64_INS_SABDL2 ||
         Insn->id == AARCH64_INS_SABAL || Insn->id == AARCH64_INS_SABAL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SABDL2 || Insn->id == AARCH64_INS_UABDL2 ||
         Insn->id == AARCH64_INS_SABAL2 || Insn->id == AARCH64_INS_UABAL2);
    bool IsAccum =
        (Insn->id == AARCH64_INS_SABAL || Insn->id == AARCH64_INS_SABAL2 ||
         Insn->id == AARCH64_INS_UABAL || Insn->id == AARCH64_INS_UABAL2);
    unsigned DstLane = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_8H:
      DstLane = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
      DstLane = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
      DstLane = 8;
      break;
    default:
      break;
    }
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned NarrowBase = IsUpper ? 8 : 0;
      NdVar OldD = IsAccum ? operandRead(S, ARM64.operands[0]) : NdVar();
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA,
               {A, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
        NdVar La = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, La, {NarrA});
        NdVar NarrB = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrB,
               {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
        NdVar Lb = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        NdVar Diff = S.makeTemp(DstLane);
        S.emit(NdOp::INT_SUB, Diff, {La, Lb});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {La, Lb});
        NdVar NegDiff = S.makeTemp(DstLane);
        S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
        NdVar AbsDiff = S.makeTemp(DstLane);
        S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
        NdVar LaneRes = AbsDiff;
        if (IsAccum) {
          NdVar OldLane = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, OldLane, {OldD, NdVar::cst(I * DstLane, 4)});
          LaneRes = S.makeTemp(DstLane);
          S.emit(NdOp::INT_ADD, LaneRes, {OldLane, AbsDiff});
        }
        if (I == 0) {
          Acc = LaneRes;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Diff = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Diff, {A, B});
      if (IsAccum)
        S.emit(NdOp::INT_ADD, Dst, {Dst, Diff});
      else
        S.emit(NdOp::COPY, Dst, {Diff});
    }
    break;
  }
  case AARCH64_INS_SMAXP:
  case AARCH64_INS_SMINP:
  case AARCH64_INS_UMAXP:
  case AARCH64_INS_UMINP: {
    // Pairwise min/max: concatenate A then B, reduce adjacent pairs.  The lanes
    // from A occupy the low half of the result, B's the high half.  The old
    // `COPY Dst,A` placeholder dropped B and never reduced the pairs.
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SMAXP || Insn->id == AARCH64_INS_SMINP);
    bool IsMax =
        (Insn->id == AARCH64_INS_SMAXP || Insn->id == AARCH64_INS_UMAXP);
    unsigned LaneSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
      LaneSz = 8;
      break;
    default:
      break;
    }
    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          NdVar Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Cmp = S.makeTemp(1);
          if (IsMax)
            S.emit(CmpOp, Cmp, {Hi, Lo});
          else
            S.emit(CmpOp, Cmp, {Lo, Hi});
          NdVar Sel = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, Sel, {Cmp, Lo, Hi});
          unsigned Idx = H * NPairs + I;
          if (Idx == 0) {
            Acc = Sel;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Sel, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {A});
    }
    break;
  }

  // Float conditional compare
  case AARCH64_INS_FCCMP:
  case AARCH64_INS_FCCMPE: {
    if (ARM64.op_count < 3)
      break;
    NdVar A = operandRead(S, ARM64.operands[0]);
    NdVar B = operandRead(S, ARM64.operands[1]);
    uint64_t NZCVImm = 0;
    if (ARM64.operands[2].type == AARCH64_OP_IMM)
      NZCVImm = ARM64.operands[2].imm;

    NdVar CmpN = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, CmpN, {A, B});
    NdVar CmpZ = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, CmpZ, {A, B});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {A, B});
    NdVar CmpC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, CmpC, {Lt});
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {A});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {B});
    NdVar CmpV = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, CmpV, {NanA, NanB});

    NdVar Cond = S.makeTemp(1);
    switch (ARM64.cc) {
    case AArch64CC_EQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_NE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_HS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_LO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_MI:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_PL:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_VS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_VC:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_LT:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GT: {
      NdVar NZ = S.makeTemp(1);
      NdVar EqFlags = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, EqFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, EqFlags});
      break;
    }
    case AArch64CC_LE: {
      NdVar NeFlags = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, NeFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NeFlags});
      break;
    }
    case AArch64CC_HI: {
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(a64reg::CFLAG, 1), NZ});
      break;
    }
    case AArch64CC_LS: {
      NdVar NC = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NC});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }

    NdVar ImmN = NdVar::cst((NZCVImm >> 3) & 1, 1);
    NdVar ImmZ = NdVar::cst((NZCVImm >> 2) & 1, 1);
    NdVar ImmC = NdVar::cst((NZCVImm >> 1) & 1, 1);
    NdVar ImmV = NdVar::cst(NZCVImm & 1, 1);
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::NFLAG, 1), {Cond, CmpN, ImmN});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::ZFLAG, 1), {Cond, CmpZ, ImmZ});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::CFLAG, 1), {Cond, CmpC, ImmC});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::VFLAG, 1), {Cond, CmpV, ImmV});
    break;
  }

  // Float: FABD, FJCVTZS, FCVTXN, FRINT32/64
  case AARCH64_INS_FABD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    // Vector `fabd v.4s/.2d` is PER-LANE |A[i]-B[i]|; a single FLOAT_SUB/ABS on
    // the whole i128 makes the emitter treat 16 bytes as one FP value and drops
    // lanes (a64v8_fabs lost half its elements).
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2; // half-precision (FEAT_FP16) vectors are also per-lane
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar D = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_SUB, D, {La, Lb});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_ABS, R, {D});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Diff = S.makeTemp(Dst.Size);
      S.emit(NdOp::FLOAT_SUB, Diff, {A, B});
      S.emit(NdOp::FLOAT_ABS, Dst, {Diff});
    }
    break;
  }
  // FJCVTZS (FEAT_JSCVT): JavaScript convert double->int32, round toward zero
  // with modulo-2^32 WRAP on overflow (NaN/Inf -> 0).  A plain FLOAT_TRUNC
  // (FPToSI) instead SATURATES out-of-range / Inf inputs, so the two diverge
  // outside [-2^31, 2^31).  Map to the real llvm.aarch64.fjcvtzs intrinsic so
  // codegen emits `fjcvtzs` and the recompiled code is bit-exact under Unicorn.
  // (The PSTATE.Z "inexact" flag side effect is not modelled -- no roundtrip
  // covers it, same as the previous lift.)
  case AARCH64_INS_FJCVTZS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Fjcvtzs, Dst, {Src});
    break;
  }
  // FCVTXN/FCVTXN2: FP inexact narrowing f64->f32 with round-to-ODD (jamming),
  // not round-to-nearest-even.  A plain FLOAT_FLOAT2FLOAT rounds to even, which
  // differs on every inexact narrowing; the "2" form additionally writes the
  // narrowed pair into the HIGH 64 bits of Vd (keeping the low half).  Map to
  // the real sisd/neon fcvtxn intrinsic (round-to-odd, per-lane) so codegen
  // emits `fcvtxn` and the recompiled code is bit-exact under Unicorn.
  case AARCH64_INS_FCVTXN:
  case AARCH64_INS_FCVTXN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (Insn->id == AARCH64_INS_FCVTXN2) {
      // Narrow the two f64 source lanes to two f32 and place them in the HIGH
      // 64 bits of Vd, preserving the existing low 64 bits.
      NdVar OldVd = operandRead(S, ARM64.operands[0]);
      NdVar Narrow = S.makeTemp(8);
      S.emitIntrinsic(Intrinsic::A64_Fcvtxn, Narrow, {Src});
      NdVar Low = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Low, {OldVd, NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Narrow, Low});
    } else {
      S.emitIntrinsic(Intrinsic::A64_Fcvtxn, Dst, {Src});
    }
    break;
  }
  // FRINT32Z/FRINT32X/FRINT64Z/FRINT64X (FEAT_FRINTTS): round each f32/f64 lane
  // to an integral float, then clamp to the signed 32-bit (FRINT32*) or 64-bit
  // (FRINT64*) integer range.  Out-of-range, NaN or Inf inputs yield INT_MIN as
  // a float (-2^31 for FRINT32*, -2^63 for FRINT64*); the "Z" forms round
  // toward zero, the "X" forms use the FPCR rounding mode (default
  // round-to-nearest- even).  The old code used a single whole-register
  // FLOAT_ROUND, which is wrong three ways: ties-away rounding (not toward-zero
  // / to-even), no range clamping, and -- for vectors -- it collapsed all lanes
  // into one FP value.
  // +/-2^31 and +/-2^63 are exactly representable in both f32 and f64, so the
  // float-comparison clamp matches QEMU's frint_s/frint_d exponent check bit-
  // exactly.
  case AARCH64_INS_FRINT32X:
  case AARCH64_INS_FRINT32Z:
  case AARCH64_INS_FRINT64X:
  case AARCH64_INS_FRINT64Z: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    bool IsZ =
        (Insn->id == AARCH64_INS_FRINT32Z || Insn->id == AARCH64_INS_FRINT64Z);
    bool Is32 =
        (Insn->id == AARCH64_INS_FRINT32X || Insn->id == AARCH64_INS_FRINT32Z);
    auto frintLane = [&](NdVar Lane, unsigned Sz) -> NdVar {
      // 1) Round to an integral float in the requested mode.
      NdVar R = S.makeTemp(Sz);
      if (IsZ) {
        // Round toward zero = floor for non-negative, ceil for negative.
        NdVar Fl = S.makeTemp(Sz), Ce = S.makeTemp(Sz), Neg = S.makeTemp(1);
        S.emit(NdOp::FLOAT_FLOOR, Fl, {Lane});
        S.emit(NdOp::FLOAT_CEIL, Ce, {Lane});
        S.emit(NdOp::FLOAT_LESS, Neg, {Lane, NdVar::cst(0, Sz)});
        S.emit(NdOp::SELECT, R, {Neg, Ce, Fl});
      } else {
        S.emit(NdOp::FLOAT_ROUNDEVEN, R, {Lane});
      }
      // 2) Bit patterns for +/-2^(N-1) at this lane width (N = 32 or 64).
      uint64_t BoundPos, BoundNeg;
      if (Sz == 4) {
        BoundPos = Is32 ? 0x4F000000ULL : 0x5F000000ULL; // 2^31 / 2^63 (f32)
        BoundNeg = Is32 ? 0xCF000000ULL : 0xDF000000ULL; // -2^31 / -2^63
      } else {
        BoundPos = Is32 ? 0x41E0000000000000ULL  // 2^31 (f64)
                        : 0x43E0000000000000ULL; // 2^63 (f64)
        BoundNeg = Is32 ? 0xC1E0000000000000ULL  // -2^31
                        : 0xC3E0000000000000ULL; // -2^63
      }
      // 3) Overflow if NaN/Inf, or the rounded result is out of
      //    [-2^(N-1), 2^(N-1)).  (Inf is caught by the magnitude checks.)
      NdVar IsNan = S.makeTemp(1);
      S.emit(NdOp::FLOAT_ISNAN, IsNan, {Lane});
      NdVar Hi = S.makeTemp(1); // r >= BoundPos  <=>  BoundPos <= r
      S.emit(NdOp::FLOAT_LESSEQUAL, Hi, {NdVar::cst(BoundPos, Sz), R});
      NdVar Lo = S.makeTemp(1); // r < BoundNeg
      S.emit(NdOp::FLOAT_LESS, Lo, {R, NdVar::cst(BoundNeg, Sz)});
      NdVar Ovf1 = S.makeTemp(1), Ovf = S.makeTemp(1);
      S.emit(NdOp::BOOL_OR, Ovf1, {IsNan, Hi});
      S.emit(NdOp::BOOL_OR, Ovf, {Ovf1, Lo});
      // 4) INT_MIN (as float, == BoundNeg) on overflow, else the rounded value.
      NdVar Out = S.makeTemp(Sz);
      S.emit(NdOp::SELECT, Out, {Ovf, NdVar::cst(BoundNeg, Sz), R});
      return Out;
    };
    auto DstVas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S || DstVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar R = frintLane(Lane, LaneSz);
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {frintLane(Src, Dst.Size)});
    }
    break;
  }

  // FMLAL/FMLSL/FMLAL2/FMLSL2 (FEAT_FHM): fp16->fp32 widening fused multiply-
  // add/subtract.  Each f32 destination lane i accumulates
  // fma(+/-widen(Vn.h[j]), widen(Vm.h[k]), Vd[i]) with a SINGLE rounding.  The
  // "2" variants read the HIGH NLanes fp16 lanes of the source register(s); the
  // by-element form broadcasts one fp16 lane of Vm (index 0..7 of the full
  // reg). The old code used a naive whole-register FLOAT_MULT + separate
  // FLOAT_ADD/SUB (no fp16->fp32 widening, no per-lane structure, double
  // rounding).
  case AARCH64_INS_FMLAL:
  case AARCH64_INS_FMLSL:
  case AARCH64_INS_FMLAL2:
  case AARCH64_INS_FMLSL2: {
    if (ARM64.op_count < 3)
      break;
    bool IsSub =
        (Insn->id == AARCH64_INS_FMLSL || Insn->id == AARCH64_INS_FMLSL2);
    bool IsHigh =
        (Insn->id == AARCH64_INS_FMLAL2 || Insn->id == AARCH64_INS_FMLSL2);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);
    unsigned NLanes = Dst.Size / 4;             // f32 destination lanes
    int BLane = ARM64.operands[2].vector_index; // >=0 for the by-element form
    // The "2" variants reach fp16 lanes NLanes..2*NLanes-1, which live in the
    // high half of the V register; the .4h/.2h operand view is only 8/4 bytes,
    // so re-read the full 16-byte register and offset the lane base.
    NdVar ASrc = A, BSrc = B;
    unsigned ALaneBase = 0, BLaneBase = 0;
    if (IsHigh) {
      ASrc = NdVar::reg(A.Offset, 16);
      ALaneBase = NLanes;
      if (BLane < 0) { // vector form: Vm also uses the high lanes
        BSrc = NdVar::reg(B.Offset, 16);
        BLaneBase = NLanes;
      }
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Ah = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, Ah, {ASrc, NdVar::cst((ALaneBase + I) * 2, 4)});
      NdVar Bh;
      if (BLane >= 0 && B.Size <= 2) {
        Bh = B; // operandRead already returned the single indexed fp16 lane
      } else {
        unsigned BOff = (BLane >= 0 ? (unsigned)BLane : (BLaneBase + I)) * 2;
        Bh = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Bh, {BSrc, NdVar::cst(BOff, 4)});
      }
      NdVar Wa = S.makeTemp(4), Wb = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Wa, {Ah}); // fp16 -> fp32 widen
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Wb, {Bh});
      if (IsSub) {
        NdVar NWa = S.makeTemp(4);
        S.emit(NdOp::FLOAT_NEG, NWa, {Wa});
        Wa = NWa;
      }
      NdVar Dl = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Dl, {DstR, NdVar::cst(I * 4, 4)});
      NdVar R = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FMA, R, {Wa, Wb, Dl}); // single rounding (fused)
      if (I == 0) {
        Acc = R;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 4);
        S.emit(NdOp::CONCAT, Next, {R, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // The FEAT_FP8 variants (FMLALB/FMLALT/FMLALL*, FMLSLB/FMLSLT) widen fp8 (not
  // fp16) with different lane selection and are not yet modelled; keep the
  // naive behaviour pending a dedicated pass.  (FMLAL2/FMLSL2 are handled above
  // by the fp16 FEAT_FHM widening path.)
  case AARCH64_INS_FMLALB:
  case AARCH64_INS_FMLALT:
  case AARCH64_INS_FMLALL:
  case AARCH64_INS_FMLALLBB:
  case AARCH64_INS_FMLALLBT:
  case AARCH64_INS_FMLALLTB:
  case AARCH64_INS_FMLALLTT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_FMLSLB:
  case AARCH64_INS_FMLSLT: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {Dst, Prod});
    break;
  }
  case AARCH64_INS_FMMLA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Prod});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
