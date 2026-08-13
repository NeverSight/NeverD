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
