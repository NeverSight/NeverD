//===- X86LiftCoreMove.cpp - x86/x64 data movement lifter -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NOP, the MOV family including the zero/sign-extending and
/// SSE register-to-register forms, the high/low quadword
/// moves, scalar integer/float conversion, LEA and XCHG.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

int movrsGprIndex(x86_reg Reg, unsigned Width) {
  static const x86_reg Low8[] = {X86_REG_AL, X86_REG_CL, X86_REG_DL,
                                 X86_REG_BL, X86_REG_SPL, X86_REG_BPL,
                                 X86_REG_SIL, X86_REG_DIL};
  static const x86_reg Low16[] = {X86_REG_AX, X86_REG_CX, X86_REG_DX,
                                  X86_REG_BX, X86_REG_SP, X86_REG_BP,
                                  X86_REG_SI, X86_REG_DI};
  static const x86_reg Low32[] = {X86_REG_EAX, X86_REG_ECX, X86_REG_EDX,
                                  X86_REG_EBX, X86_REG_ESP, X86_REG_EBP,
                                  X86_REG_ESI, X86_REG_EDI};
  static const x86_reg Low64[] = {X86_REG_RAX, X86_REG_RCX, X86_REG_RDX,
                                  X86_REG_RBX, X86_REG_RSP, X86_REG_RBP,
                                  X86_REG_RSI, X86_REG_RDI};
  const x86_reg *Low = Width == 1 ? Low8 : Width == 2 ? Low16
                                      : Width == 4   ? Low32
                                                     : Low64;
  for (unsigned I = 0; I != 8; ++I)
    if (Reg == Low[I])
      return static_cast<int>(I);
  if (Width == 1) {
    if (Reg >= X86_REG_R8B && Reg <= X86_REG_R15B)
      return 8 + static_cast<int>(Reg - X86_REG_R8B);
    if (Reg >= X86_REG_R16B && Reg <= X86_REG_R31B)
      return 16 + static_cast<int>(Reg - X86_REG_R16B);
  } else if (Width == 2) {
    if (Reg >= X86_REG_R8W && Reg <= X86_REG_R15W)
      return 8 + static_cast<int>(Reg - X86_REG_R8W);
    if (Reg >= X86_REG_R16W && Reg <= X86_REG_R31W)
      return 16 + static_cast<int>(Reg - X86_REG_R16W);
  } else if (Width == 4) {
    if (Reg >= X86_REG_R8D && Reg <= X86_REG_R15D)
      return 8 + static_cast<int>(Reg - X86_REG_R8D);
    if (Reg >= X86_REG_R16D && Reg <= X86_REG_R31D)
      return 16 + static_cast<int>(Reg - X86_REG_R16D);
  } else if (Width == 8) {
    if (Reg >= X86_REG_R8 && Reg <= X86_REG_R15)
      return 8 + static_cast<int>(Reg - X86_REG_R8);
    if (Reg >= X86_REG_R16 && Reg <= X86_REG_R31)
      return 16 + static_cast<int>(Reg - X86_REG_R16);
  }
  return -1;
}

x86_reg movrsSegment(uint8_t Prefix) {
  switch (Prefix) {
  case 0x26: return X86_REG_ES;
  case 0x2e: return X86_REG_CS;
  case 0x36: return X86_REG_SS;
  case 0x3e: return X86_REG_DS;
  case 0x64: return X86_REG_FS;
  case 0x65: return X86_REG_GS;
  default: return X86_REG_INVALID;
  }
}

bool movrsAddressRegMatches(x86_reg Reg, unsigned Number, unsigned Width) {
  return movrsGprIndex(Reg, Width) == static_cast<int>(Number);
}

bool validateApxMovrs(const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn || X86.op_count != 2 || X86.operands[0].type != X86_OP_REG ||
      X86.operands[1].type != X86_OP_MEM ||
      X86.operands[0].access != CS_AC_WRITE ||
      X86.operands[1].access != CS_AC_READ ||
      X86.operands[0].size != X86.operands[1].size)
    return false;
  const unsigned Width = X86.operands[0].size;
  if (Width != 1 && Width != 2 && Width != 4 && Width != 8)
    return false;

  size_t E = 0;
  uint8_t SegmentPrefix = 0;
  bool Address32 = false;
  while (E < Insn->size && Insn->bytes[E] != 0x62) {
    const uint8_t P = Insn->bytes[E++];
    if (P == 0x67) {
      if (Address32)
        return false;
      Address32 = true;
    } else if (movrsSegment(P) != X86_REG_INVALID) {
      if (SegmentPrefix != 0)
        return false;
      SegmentPrefix = P;
    } else {
      return false;
    }
  }
  if (E + 6 > Insn->size || Insn->bytes[E] != 0x62 ||
      X86.encoding.modrm_offset != E + 5 || X86.encoding.imm_size != 0 ||
      X86.modrm != Insn->bytes[E + 5] || (X86.modrm & 0xc0) == 0xc0 ||
      X86.addr_size != (Address32 ? 4 : 8) ||
      X86.prefix[1] != SegmentPrefix ||
      X86.prefix[3] != (Address32 ? 0x67 : 0) ||
      X86.operands[1].mem.segment != movrsSegment(SegmentPrefix))
    return false;
  const uint8_t P0 = Insn->bytes[E + 1], P1 = Insn->bytes[E + 2];
  const uint8_t P2 = Insn->bytes[E + 3], Opcode = Insn->bytes[E + 4];
  if ((P0 & 7) != 4 || (P1 & 0x78) != 0x78 || P2 != 8 ||
      (Opcode != 0x8a && Opcode != 0x8b))
    return false;
  unsigned EncodedWidth = 0;
  if (Opcode == 0x8a && (P1 & 0x83) == 0)
    EncodedWidth = 1;
  else if (Opcode == 0x8b && (P1 & 3) == 1 && !(P1 & 0x80))
    EncodedWidth = 2;
  else if (Opcode == 0x8b && (P1 & 3) == 0)
    EncodedWidth = (P1 & 0x80) ? 8 : 4;
  if (EncodedWidth != Width)
    return false;
  const unsigned EncodedReg = ((~P0 & 0x80) >> 4) | (~P0 & 0x10) |
                              ((X86.modrm >> 3) & 7);
  if (movrsGprIndex(static_cast<x86_reg>(X86.operands[0].reg), Width) !=
      static_cast<int>(EncodedReg))
    return false;

  const unsigned AddressWidth = Address32 ? 4 : 8;
  const unsigned BaseExtension = ((~P0 & 0x20) >> 2) | ((P0 & 0x08) << 1);
  const unsigned IndexExtension = ((~P0 & 0x40) >> 3) |
                                  ((~P1 & 0x04) << 2);
  const unsigned Mod = X86.modrm >> 6, Rm = X86.modrm & 7;
  size_t Cursor = E + 6;
  x86_reg ExpectedBase = X86_REG_INVALID;
  x86_reg ExpectedIndex = X86_REG_INVALID;
  unsigned ExpectedScale = 1;
  uint8_t DispSize = 0;
  if (Rm == 4) {
    if (Cursor >= Insn->size || X86.sib != Insn->bytes[Cursor])
      return false;
    const uint8_t Sib = Insn->bytes[Cursor++];
    ExpectedScale = 1u << (Sib >> 6);
    const unsigned Index = (Sib >> 3) & 7, Base = Sib & 7;
    if (Index != 4 || IndexExtension != 0) {
      ExpectedIndex = static_cast<x86_reg>(X86.operands[1].mem.index);
      if (!movrsAddressRegMatches(ExpectedIndex, Index + IndexExtension,
                                  AddressWidth))
        return false;
    }
    if (Mod == 0 && Base == 5)
      DispSize = 4;
    else {
      ExpectedBase = static_cast<x86_reg>(X86.operands[1].mem.base);
      if (!movrsAddressRegMatches(ExpectedBase, Base + BaseExtension,
                                  AddressWidth))
        return false;
    }
  } else if (Mod == 0 && Rm == 5) {
    ExpectedBase = Address32 ? X86_REG_EIP : X86_REG_RIP;
    DispSize = 4;
  } else {
    ExpectedBase = static_cast<x86_reg>(X86.operands[1].mem.base);
    if (!movrsAddressRegMatches(ExpectedBase, Rm + BaseExtension,
                                AddressWidth))
      return false;
  }
  if (Mod == 1)
    DispSize = 1;
  else if (Mod == 2)
    DispSize = 4;
  int64_t Disp = 0;
  const size_t DispOffset = Cursor;
  if (DispSize == 1) {
    if (Cursor >= Insn->size)
      return false;
    Disp = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (DispSize == 4) {
    if (Cursor + 4 > Insn->size)
      return false;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 4; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor++]) << (I * 8);
    Disp = static_cast<int32_t>(Raw);
  }
  const auto &Mem = X86.operands[1].mem;
  if (Cursor != Insn->size || Mem.base != ExpectedBase ||
      Mem.index != ExpectedIndex || Mem.scale != static_cast<int>(ExpectedScale) ||
      Mem.disp != Disp || X86.encoding.disp_size != DispSize ||
      X86.encoding.disp_offset != (DispSize ? DispOffset : 0))
    return false;
  return true;
}

} // namespace

bool liftCoreMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_MOVRS: {
    if (!validateApxMovrs(Insn, X86))
      return false;
    // MOVRS is a restricted-speculation hint, not a different architectural
    // load.  It returns the same bytes and raises the same memory faults as an
    // ordinary load.  Emitting LOAD before COPY preserves fault atomicity: a
    // failing memory read cannot write the destination register.
    NdVar Value = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Value});
    break;
  }

  // --- NOP ---
  case X86_INS_NOP:
  case X86_INS_FNOP:
    S.emit(NdOp::NOP, {}, {});
    break;

  // --- MOV (+ MOVABS, plus SSE/SIMD 128-bit register-to-register moves).
  // MOVAPS/MOVUPS/MOVDQA/MOVDQU/MOVD/MOVQ semantically are just "copy bytes"
  // for our purposes — the lift doesn't model float arithmetic separately
  // unless we later promote to FLOAT_* S.Ops. Treating them as bulk COPY at
  // least preserves dataflow (so the destination XMM reg has a defined
  // value) instead of dropping the instruction to NOP.
  case X86_INS_MOV:
  case X86_INS_MOVABS:
  case X86_INS_MOVZX:
  case X86_INS_MOVSX:
  case X86_INS_MOVSXD:
  case X86_INS_MOVAPS:
  case X86_INS_MOVAPD:
  case X86_INS_MOVUPS:
  case X86_INS_MOVUPD:
  case X86_INS_MOVDQA:
  case X86_INS_MOVDQU:
  case X86_INS_MOVD:
  case X86_INS_MOVQ:
  case X86_INS_MOVSS: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar DstV = L.operandWrite(X86.operands[0]);

    if (InsnId == X86_INS_MOVZX) {
      NdVar Ext = S.makeTemp(DstV.Size);
      S.emit(NdOp::INT_ZEXT, Ext, {Src});
      Src = Ext;
    } else if (InsnId == X86_INS_MOVSX || InsnId == X86_INS_MOVSXD) {
      NdVar Ext = S.makeTemp(DstV.Size);
      S.emit(NdOp::INT_SEXT, Ext, {Src});
      Src = Ext;
    }

    if (X86.operands[0].type == X86_OP_MEM) {
      // Scalar FP stores: truncate XMM to actual operand width
      if (InsnId == X86_INS_MOVSD && Src.Size > 8) {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      } else if ((InsnId == X86_INS_MOVSS || InsnId == X86_INS_MOVD) &&
                 Src.Size > 4) {
        NdVar Lo = S.makeTemp(4);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      } else if (InsnId == X86_INS_MOVQ && Src.Size > 8) {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.storeToMem(X86.operands[0], Src);
    } else {
      if (Src.Size > DstV.Size) {
        NdVar Lo = S.makeTemp(DstV.Size);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }

      // A narrow source moved into a wider XMM/MMX register zero-extends the
      // upper lanes for MOVD/MOVQ and for the MEMORY forms of MOVSS/MOVSD
      // (Intel SDM); the register forms of MOVSS/MOVSD merge instead (handled
      // below).  Make the zero extension explicit so the destination is a true
      // full-width def — a plain COPY of the narrow value leaves the upper
      // bytes stale, which a later full-width read (e.g. a movdqa spill that is
      // reloaded by psadbw) would otherwise expose as garbage.
      // (MOVSD reaches the dedicated handler in X86LiftString.cpp, not here.)
      bool ZeroExtendsUpper =
          (InsnId == X86_INS_MOVD || InsnId == X86_INS_MOVQ) ||
          (InsnId == X86_INS_MOVSS && X86.operands[1].type == X86_OP_MEM);
      if (Src.Size < DstV.Size && ZeroExtendsUpper) {
        NdVar Ext = S.makeTemp(DstV.Size);
        S.emit(NdOp::INT_ZEXT, Ext, {Src});
        Src = Ext;
      }

      // MOVSS/MOVSD reg-to-reg: merges only the low scalar element into
      // the destination, preserving its upper bits.  Other MOV variants
      // (MOVAPS, MOVDQA, etc.) are full copies.
      bool NeedsMerge = (InsnId == X86_INS_MOVSS || InsnId == X86_INS_MOVSD) &&
                        X86.operands[1].type == X86_OP_REG &&
                        DstV.Size > Src.Size;
      if (NeedsMerge) {
        // This is MOVSS/MOVSD from xmm to xmm — scalar merge.
        // But if Src was already truncated above, Src.Size == DstV.Size,
        // so NeedsMerge wouldn't trigger.  Handle the case where
        // both operands report full XMM size.
      }

      if ((InsnId == X86_INS_MOVSS || InsnId == X86_INS_MOVSD) &&
          X86.operands[0].type == X86_OP_REG &&
          X86.operands[1].type == X86_OP_REG && DstV.Size >= 16) {
        uint16_t ElemSz = (InsnId == X86_INS_MOVSS) ? 4 : 8;
        NdVar DstR = L.operandRead(S, X86.operands[0]);
        NdVar SrcLo = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, SrcLo, {Src, NdVar::cst(0, 4)});
        NdVar DstHi = S.makeTemp(DstV.Size - ElemSz);
        S.emit(NdOp::SUBBYTES, DstHi, {DstR, NdVar::cst(ElemSz, 4)});
        S.emit(NdOp::CONCAT, DstV, {DstHi, SrcLo});
      } else {
        S.emit(NdOp::COPY, DstV, {Src});
      }
    }
    break;
  }

  // MOVLPS/MOVLPD/MOVHPS/MOVHPD — partial 64-bit moves: each touches ONE half
  // of the XMM and must preserve / select the correct other half.
  //   store: movlps m64,xmm → m64 = xmm[63:0];  movhps m64,xmm → m64 =
  //   xmm[127:64] load:  movlps xmm,m64 → xmm[63:0]=m64, [127:64] kept
  //          movhps xmm,m64 → xmm[127:64]=m64, [63:0] kept
  // The old code shared the generic MOV path: stores always took the LOW qword
  // (so MOVHPS/MOVHPD stored the wrong half) and loads did a flat COPY of the
  // 8-byte memory value into the 16-byte XMM (clobbering the preserved half).
  case X86_INS_MOVLPS:
  case X86_INS_MOVLPD:
  case X86_INS_MOVHPS:
  case X86_INS_MOVHPD: {
    if (X86.op_count < 2)
      break;
    bool IsHigh = (InsnId == X86_INS_MOVHPS || InsnId == X86_INS_MOVHPD);
    if (X86.operands[0].type == X86_OP_MEM) {
      NdVar Src = L.operandRead(S, X86.operands[1]);
      NdVar Half = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(IsHigh ? 8 : 0, 4)});
      S.storeToMem(X86.operands[0], Half);
    } else {
      NdVar DstV = L.operandWrite(X86.operands[0]);
      NdVar DstR = L.operandRead(S, X86.operands[0]);
      NdVar Mem = L.operandRead(S, X86.operands[1]);
      NdVar MemLo = Mem;
      if (Mem.Size != 8) {
        MemLo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, MemLo, {Mem, NdVar::cst(0, 4)});
      }
      if (IsHigh) {
        // high = m64, low preserved → {lo, m64}.
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {DstR, NdVar::cst(0, 4)});
        S.emit(NdOp::CONCAT, DstV, {MemLo, Lo});
      } else {
        // low = m64, high preserved → {m64, hi}.
        NdVar Hi = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Hi, {DstR, NdVar::cst(8, 4)});
        S.emit(NdOp::CONCAT, DstV, {Hi, MemLo});
      }
    }
    break;
  }

  // --- FP conversions (scalar) ---
  case X86_INS_CVTTSD2SI:
  case X86_INS_CVTTSS2SI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    unsigned FPSz = (Insn->id == X86_INS_CVTTSS2SI) ? 4 : 8;
    if (Src.Size > FPSz) {
      NdVar Narrow = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
      Src = Narrow;
    }
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }
  case X86_INS_CVTSD2SI:
  case X86_INS_CVTSS2SI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    unsigned FPSz = (Insn->id == X86_INS_CVTSS2SI) ? 4 : 8;
    if (Src.Size > FPSz) {
      NdVar Narrow = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
      Src = Narrow;
    }
    // CVTSD2SI/CVTSS2SI round using MXCSR (default: nearest, ties to even),
    // unlike the truncating CVTTSD2SI/CVTTSS2SI.  Round first, then convert.
    NdVar Rounded = S.makeTemp(FPSz);
    S.emit(NdOp::FLOAT_ROUNDEVEN, Rounded, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Rounded});
    break;
  }
  case X86_INS_CVTSI2SD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Tmp = S.makeTemp(8);
    S.emit(NdOp::FLOAT_INT2FLOAT, Tmp, {Src});
    // NOTE: this writes the low double as a narrow (8-byte) scalar value and
    // does NOT rebuild the destination's upper 64 bits.  That is deliberate and
    // matches the codebase-wide scalar-SSE convention: a scalar XMM value is
    // kept narrow-typed so downstream scalar ops (sqrtss/mulss/comiss/...)
    // infer the right FP width.  Forcing a 16-byte CONCAT here to preserve
    // xmm[127:64] re-types the register as i128, and inferFloatTy() then
    // mis-selects double for those consumers.  Real
    // compilers never read the upper lane after CVTSI2*, so the divergence is
    // unobservable in practice; only hand-written asm can see it.
    S.emit(NdOp::COPY, Dst, {Tmp});
    break;
  }
  case X86_INS_CVTSI2SS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Tmp = S.makeTemp(4);
    S.emit(NdOp::FLOAT_INT2FLOAT, Tmp, {Src});
    // See CVTSI2SD above: narrow scalar write by design; upper lanes are not
    // rebuilt to keep the value float-typed for downstream scalar consumers.
    S.emit(NdOp::COPY, Dst, {Tmp});
    break;
  }
  case X86_INS_CVTSD2SS:
  case X86_INS_CVTSS2SD: {
    if (X86.op_count < 2)
      break;
    bool ToDouble = (Insn->id == X86_INS_CVTSS2SD);
    unsigned SrcFPSz = ToDouble ? 4 : 8;
    unsigned DstFPSz = ToDouble ? 8 : 4;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    // Narrow the source to its real scalar FP width: a full XMM (16 bytes)
    // would make the emitter mis-infer the source type and pick the wrong
    // conversion direction.
    if (Src.Size > SrcFPSz) {
      NdVar N = S.makeTemp(SrcFPSz);
      S.emit(NdOp::SUBBYTES, N, {Src, NdVar::cst(0, 4)});
      Src = N;
    }
    NdVar Tmp = S.makeTemp(DstFPSz);
    S.emit(NdOp::FLOAT_FLOAT2FLOAT, Tmp, {Src});
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Tmp);
    } else if (Dst.Size > DstFPSz) {
      // Scalar SSE conversions preserve the upper lanes of the destination.
      NdVar Hi = S.makeTemp(Dst.Size - DstFPSz);
      S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::cst(DstFPSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Tmp});
    } else {
      S.emit(NdOp::COPY, Dst, {Tmp});
    }
    break;
  }

  // --- LEA ---
  case X86_INS_LEA: {
    if (X86.op_count < 2)
      break;
    NdVar DstV = L.operandWrite(X86.operands[0]);
    // LEA computes only the offset expression; segment overrides never add a
    // segment base and therefore do not select a segmented memory space.
    NdVar EA = S.computeEA(X86.operands[1], /*ForMemoryAccess=*/false);
    if (EA.Size > DstV.Size) {
      NdVar Trunc = S.makeTemp(DstV.Size);
      S.emit(NdOp::SUBBYTES, Trunc, {EA, NdVar::cst(0, 4)});
      EA = Trunc;
    }
    S.emit(NdOp::COPY, DstV, {EA});
    break;
  }

  // --- XCHG ---
  case X86_INS_XCHG: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar TmpV = S.makeTemp(A.Size);
    S.emit(NdOp::COPY, TmpV, {A});
    // A MEMORY operand must be written with an explicit STORE —
    // L.operandWrite() returns a discarded ram(0) placeholder, so `xchg
    // [mem],reg` previously updated only the register and silently dropped the
    // memory half.  Do the store FIRST (using the original register values for
    // its address) before the register write, so a register that also indexes
    // the address (e.g. `xchg rax,[rax]`) is not clobbered first.
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], B);
      S.emit(NdOp::COPY, L.operandWrite(X86.operands[1]), {TmpV});
    } else if (X86.operands[1].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[1], TmpV);
      S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {B});
    } else {
      S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {B});
      S.emit(NdOp::COPY, L.operandWrite(X86.operands[1]), {TmpV});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
