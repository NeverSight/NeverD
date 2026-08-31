//===- X86LiftSIMDCrypto.cpp - x86/x64 SIMD crypto and extension lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cryptographic acceleration (AES-NI, SHA-NI, carry-less
/// multiply, CRC32) plus the transactional-memory (TSX) and
/// bounds-check (MPX) extensions that share this dispatcher.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {
unsigned vectorIndex(const cs_x86_op &Op) {
  if (Op.size == 16)
    return static_cast<unsigned>(Op.reg - X86_REG_XMM0);
  if (Op.size == 32)
    return static_cast<unsigned>(Op.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Op.reg - X86_REG_ZMM0);
}

bool validEvexCrypto(const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch,
                     uint8_t Map, uint8_t Opcode, bool HasImmediate) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != Map ||
      ((Encoding.P1 | 0x04) & 0x87) != 0x05 ||
      (Encoding.P2 & 0x97) != 0 || Encoding.Opcode != Opcode ||
      X86.op_count != (HasImmediate ? 4 : 3) ||
      X86.encoding.imm_size != (HasImmediate ? 1 : 0) || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;
  const cs_x86_op &Dst = X86.operands[0];
  const cs_x86_op &Src1 = X86.operands[1];
  const cs_x86_op &Src2 = X86.operands[2];
  if (Dst.type != X86_OP_REG || Src1.type != X86_OP_REG ||
      (Src2.type != X86_OP_REG && Src2.type != X86_OP_MEM) ||
      (Dst.size != 16 && Dst.size != 32 && Dst.size != 64) ||
      Src1.size != Dst.size || Src2.size != Dst.size)
    return false;
  const uint8_t ExpectedLength =
      Dst.size == 16 ? 0 : (Dst.size == 32 ? 0x20 : 0x40);
  if ((Encoding.P2 & 0x60) != ExpectedLength ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorIndex(Dst) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorIndex(Src1) ||
      ((Encoding.ModRM & 0xc0) == 0xc0) != (Src2.type == X86_OP_REG))
    return false;
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask ||
        X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return false;

  const size_t TrailingBytes = HasImmediate ? 1 : 0;
  if (Src2.type == X86_OP_MEM) {
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Src2, Dst.size,
                                         TrailingBytes))
      return false;
  } else if (decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorIndex(Src2) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding,
                                                TrailingBytes)) {
    return false;
  }
  if (!HasImmediate)
    return X86.encoding.imm_offset == 0;
  return X86.encoding.imm_offset == Insn->size - 1 &&
         X86.operands[3].type == X86_OP_IMM &&
         X86.operands[3].size == 1 &&
         static_cast<uint8_t>(X86.operands[3].imm) ==
             Insn->bytes[Insn->size - 1];
}
} // namespace

bool liftSIMDCrypto(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // Intel TSX (transactional memory) instructions.
  case X86_INS_XBEGIN:
  case X86_INS_XEND:
  case X86_INS_XABORT:
  case X86_INS_XTEST:
  case X86_INS_XACQUIRE:
  case X86_INS_XRELEASE: {
    if (InsnId == X86_INS_XABORT) {
      // xabort imm8 — capture the immediate so codegen can re-emit
      // `xabort $imm` (a bare `xabort` is rejected: too few operands).
      uint64_t Imm = (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM)
                         ? static_cast<uint64_t>(X86.operands[0].imm)
                         : 0;
      S.emitIntrinsic(Intrinsic::Xabort, NdVar::reg(x86reg::RAX, 8),
                      {NdVar::cst(Imm & 0xFF, 1)});
      break;
    }
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_XBEGIN:
      Id = Intrinsic::Xbegin;
      break;
    case X86_INS_XEND:
      Id = Intrinsic::Xend;
      break;
    case X86_INS_XTEST:
      Id = Intrinsic::Xtest;
      break;
    case X86_INS_XACQUIRE:
      Id = Intrinsic::Xacquire;
      break;
    case X86_INS_XRELEASE:
      Id = Intrinsic::Xrelease;
      break;
    default:
      Id = Intrinsic::Xbegin;
      break;
    }
    S.emitIntrinsic(Id);
    break;
  }

  // BNDxxx — MPX bounds (deprecated). Treat as side-effect or COPY.
  case X86_INS_BNDMK:
  case X86_INS_BNDMOV:
  case X86_INS_BNDCL:
  case X86_INS_BNDCU:
  case X86_INS_BNDCN:
  case X86_INS_BNDLDX:
  case X86_INS_BNDSTX: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  case X86_INS_AESENC:
  case X86_INS_VAESENC: {
    if (InsnId == X86_INS_VAESENC && X86.opcode[0] == 0x62 &&
        !validEvexCrypto(Insn, X86, L.targetArch(), 2, 0xdc, false))
      return false;
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesEnc, Dst, {State, Key});
    break;
  }
  case X86_INS_AESENCLAST:
  case X86_INS_VAESENCLAST: {
    if (InsnId == X86_INS_VAESENCLAST && X86.opcode[0] == 0x62 &&
        !validEvexCrypto(Insn, X86, L.targetArch(), 2, 0xdd, false))
      return false;
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesEncLast, Dst, {State, Key});
    break;
  }
  case X86_INS_AESDEC:
  case X86_INS_VAESDEC: {
    if (InsnId == X86_INS_VAESDEC && X86.opcode[0] == 0x62 &&
        !validEvexCrypto(Insn, X86, L.targetArch(), 2, 0xde, false))
      return false;
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesDec, Dst, {State, Key});
    break;
  }
  case X86_INS_AESDECLAST:
  case X86_INS_VAESDECLAST: {
    if (InsnId == X86_INS_VAESDECLAST && X86.opcode[0] == 0x62 &&
        !validEvexCrypto(Insn, X86, L.targetArch(), 2, 0xdf, false))
      return false;
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesDecLast, Dst, {State, Key});
    break;
  }
  case X86_INS_AESIMC:
  case X86_INS_VAESIMC: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesImc, Dst, {Src});
    break;
  }
  case X86_INS_AESKEYGENASSIST:
  case X86_INS_VAESKEYGENASSIST: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Intrinsic::AesKeyGenAssist, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PCLMULQDQ:
  case X86_INS_VPCLMULQDQ: {
    if (InsnId == X86_INS_VPCLMULQDQ && X86.opcode[0] == 0x62 &&
        !validEvexCrypto(Insn, X86, L.targetArch(), 3, 0x44, true))
      return false;
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    int SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar Src1 = L.operandRead(S, X86.operands[SrcIdx]);
    NdVar Src2 = L.operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Intrinsic::Pclmulqdq, Dst,
                    {Src1, Src2, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHA1RNDS4: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Sha1Rnds4, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHA1NEXTE: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha1Nexte, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA1MSG1: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha1Msg1, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA1MSG2: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha1Msg2, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA256RNDS2: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Xmm0 = NdVar::reg(x86reg::XMM0, 16);
    S.emitIntrinsic(Intrinsic::Sha256Rnds2, Dst, {Dst, Src, Xmm0});
    break;
  }
  case X86_INS_SHA256MSG1: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha256Msg1, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA256MSG2: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha256Msg2, Dst, {Dst, Src});
    break;
  }
  case X86_INS_CRC32: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    Intrinsic IC;
    switch (Src.Size) {
    case 1:
      IC = Intrinsic::X86Crc32b;
      break;
    case 2:
      IC = Intrinsic::X86Crc32w;
      break;
    case 4:
      IC = Intrinsic::X86Crc32d;
      break;
    default:
      IC = Intrinsic::X86Crc32q;
      break;
    }
    S.emitIntrinsic(IC, Dst, {Dst, Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
