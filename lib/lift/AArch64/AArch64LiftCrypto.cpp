//===- AArch64LiftCrypto.cpp - AArch64 cryptographic extension lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AES (AESE/AESD/AESMC/AESIMC), SHA1/SHA2/SHA512, SM3, SM4 and
/// BCAX handlers, plus the FMULX/FNMUL and SDOT/UDOT vector
/// extras that share this dispatch group.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftCrypto(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                const cs_insn *Insn, const cs_aarch64 &ARM64) {
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar Key = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Insn->id == AARCH64_INS_AESE ? Intrinsic::A64_Aese
                                                 : Intrinsic::A64_Aesd,
                    Dst, {OldD, Key});
    break;
  }
  case AARCH64_INS_AESMC:
  case AARCH64_INS_AESIMC: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sha1h, Dst, {Src});
    break;
  }
  case AARCH64_INS_SHA1SU1:
  case AARCH64_INS_SHA256SU0: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Insn->id == AARCH64_INS_SM3PARTW1
                        ? Intrinsic::A64_Sm3partw1
                        : Intrinsic::A64_Sm3partw2,
                    Dst, {OldD, A, B});
    break;
  }
  case AARCH64_INS_SM3SS1: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar C = L.operandRead(S, ARM64.operands[3]);
    S.emitIntrinsic(Intrinsic::A64_Sm3ss1, Dst, {A, B, C});
    break;
  }
  case AARCH64_INS_SM3TT1A:
  case AARCH64_INS_SM3TT1B:
  case AARCH64_INS_SM3TT2A:
  case AARCH64_INS_SM3TT2B: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    // operands[2] is `Vm.s[index]`; reading it directly would extract just that
    // one lane (operandRead's vector_index path).  The intrinsic needs the FULL
    // Vm vector plus the lane index as a separate ImmArg, so clear the index
    // when reading and pass it through explicitly.
    cs_aarch64_op VmOp = ARM64.operands[2];
    int Idx = VmOp.vector_index;
    VmOp.vector_index = -1;
    NdVar B = L.operandRead(S, VmOp);
    Intrinsic Id = Insn->id == AARCH64_INS_SM3TT1A   ? Intrinsic::A64_Sm3tt1a
                   : Insn->id == AARCH64_INS_SM3TT1B ? Intrinsic::A64_Sm3tt1b
                   : Insn->id == AARCH64_INS_SM3TT2A ? Intrinsic::A64_Sm3tt2a
                                                     : Intrinsic::A64_Sm3tt2b;
    S.emitIntrinsic(Id, Dst,
                    {OldD, A, B, NdVar::cst(Idx >= 0 ? (uint64_t)Idx : 0, 8)});
    break;
  }

  // SM4 (FEAT_SM4, ARMv8.2): SM4E is destructive (Vd ^= round(Vn)); SM4EKEY is
  // non-destructive key expansion (Vd = f(Vn, Vm)).  Were AesGeneric
  // placeholders.
  case AARCH64_INS_SM4E: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar OldD = L.operandRead(S, ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sm4e, Dst, {OldD, A});
    break;
  }
  case AARCH64_INS_SM4EKEY: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Intrinsic::A64_Sm4ekey, Dst, {A, B});
    break;
  }

  // BCAX — bit clear and xor. Dst = a ^ (b & ~C).
  case AARCH64_INS_BCAX: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar C = L.operandRead(S, ARM64.operands[3]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
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
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
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

  default:
    return false;
  }
  return true;
}

} // namespace neverd
