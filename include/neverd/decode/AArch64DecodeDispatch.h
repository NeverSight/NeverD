//===- AArch64DecodeDispatch.h - Native decode class dispatch -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The single entry point of the Capstone-free AArch64 operand decoder,
/// tryDecode, and the ordered table of encoding-class masks it dispatches on.
///
/// Every mask lives here rather than inside the per-class headers because the
/// order the masks are tested in is part of the decoder's meaning: some class
/// masks overlap, and a word is decoded by the first one that matches.  Each
/// per-class function assumes its mask has already matched and always returns
/// — true when it populated \p Insn, false to decline that encoding to
/// Capstone — so control never falls from one class into the next.
///
/// Include neverd/decode/AArch64NativeDecode.h instead of this header; it
/// documents the strict-subset and lift-parity contract every class obeys.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEDISPATCH_H
#define NEVERD_DECODE_AARCH64DECODEDISPATCH_H

#include "neverd/Common.h"
#include "neverd/decode/AArch64DecodeArith.h"
#include "neverd/decode/AArch64DecodeBranch.h"
#include "neverd/decode/AArch64DecodeCond.h"
#include "neverd/decode/AArch64DecodeFP.h"
#include "neverd/decode/AArch64DecodeLoadStore.h"
#include "neverd/decode/AArch64DecodeLogical.h"
#include "neverd/decode/AArch64DecodeMove.h"
#include "neverd/decode/AArch64DecodeMulDiv.h"

#include <capstone/capstone.h>
#include <cstdint>

namespace neverd {
namespace a64native {

/// Decode \p Word at \p Addr into \p Insn (+ \p Detail) if it belongs to a
/// class this native decoder covers; return true on success (Insn is fully
/// populated), false to decline (the caller must fall back to Capstone).
///
/// Covered classes (each a strict subset of Capstone's accept set, lifted
/// identically to Capstone — see the file-level contract):
///   * ADR / ADRP         (PC-relative address, immediate already resolved)
///   * MOVZ / MOVN / MOVK  (16-bit wide immediate move)
///   * ADD/SUB (immediate, shifted register, extended register) + CMP/CMN/MOV
///   * Logical (immediate + shifted register) + the MOV/MVN/TST/ORR-bitmask
///     aliases; bitfield moves (SBFM/BFM/UBFM) and EXTR/ROR
///   * Multiply-accumulate (MADD/MSUB + MUL/MNEG, SMULH/UMULH) and the widening
///     long forms (SMADDL/UMADDL/SMSUBL/UMSUBL + SMULL/UMULL/SMNEGL/UMNEGL)
///   * SDIV/UDIV and the variable shifts (LSLV/LSRV/ASRV/RORV)
///   * Data-processing 1-source integer (RBIT/REV16/REV32/REV/CLZ/CLS)
///   * Conditional select (CSEL/CSINC/CSINV/CSNEG + cset/cinc/cneg) and
///     conditional compare (CCMP/CCMN)
///   * Integer & FP/SIMD load/store (unsigned-offset, register-offset,
///     unscaled, pre/post-index) and load/store pair
///   * Scalar FP: FMOV (register, general GPR<->FP, 8-bit immediate),
///     FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FNMUL, FABS/FNEG/FSQRT,
///     FCVT (precision convert), and FCMP/FCMPE
///   * Control flow (B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/RET/BR/BLR), NOP, BRK
inline bool tryDecode(uint32_t Word, va_t Addr, cs_insn &Insn,
                      cs_detail &Detail) {
  if ((Word & 0x1F000000u) == 0x10000000u)
    return decodeAdr(Word, Addr, Insn, Detail);

  if (((Word >> 23) & 0x3Fu) == 0x25u)
    return decodeMoveWide(Word, Addr, Insn, Detail);

  if (((Word >> 24) & 0x1Fu) == 0x11u && ((Word >> 23) & 1u) == 0u)
    return decodeAddSubImm(Word, Addr, Insn, Detail);

  if (((Word >> 24) & 0x1Fu) == 0x0Bu && ((Word >> 21) & 1u) == 0u)
    return decodeAddSubShiftedReg(Word, Addr, Insn, Detail);

  if (((Word >> 24) & 0x1Fu) == 0x0Bu && ((Word >> 21) & 1u) == 1u)
    return decodeAddSubExtendedReg(Word, Addr, Insn, Detail);

  if (((Word >> 24) & 0x1Fu) == 0x0Au)
    return decodeLogicalShiftedReg(Word, Addr, Insn, Detail);

  if (((Word >> 24) & 0x7Fu) == 0x1Bu)
    return decodeDataProc3Src(Word, Addr, Insn, Detail);

  if (((Word >> 21) & 0x3FFu) == 0x0D6u)
    return decodeDataProc2Src(Word, Addr, Insn, Detail);

  if (((Word >> 21) & 0x3FFu) == 0x2D6u && ((Word >> 16) & 0x1Fu) == 0u)
    return decodeDataProc1Src(Word, Addr, Insn, Detail);

  if (((Word >> 23) & 0xFFu) == 0x27u &&
      ((Word >> 22) & 1u) == ((Word >> 31) & 1u) && ((Word >> 21) & 1u) == 0u)
    return decodeExtr(Word, Addr, Insn, Detail);

  if (((Word >> 21) & 0xFFu) == 0xD2u && ((Word >> 29) & 1u) == 1u)
    return decodeCondCompare(Word, Addr, Insn, Detail);

  if (((Word >> 21) & 0xFFu) == 0xD4u && ((Word >> 29) & 1u) == 0u &&
      ((Word >> 11) & 1u) == 0u)
    return decodeCondSelect(Word, Addr, Insn, Detail);

  if (((Word >> 23) & 0x3Fu) == 0x26u)
    return decodeBitfield(Word, Addr, Insn, Detail);

  if (((Word >> 23) & 0x3Fu) == 0x24u)
    return decodeLogicalImm(Word, Addr, Insn, Detail);

  if (Word == 0xD503201Fu)
    return decodeHintNop(Word, Addr, Insn, Detail);

  if ((Word & 0x3B000000u) == 0x39000000u && ((Word >> 26) & 1u) == 0u)
    return decodeLdStUnsignedImm(Word, Addr, Insn, Detail);

  if ((Word & 0x3B000000u) == 0x39000000u && ((Word >> 26) & 1u) == 1u)
    return decodeLdStUnsignedImmFP(Word, Addr, Insn, Detail);

  if (((Word >> 27) & 0x7u) == 0x7u && ((Word >> 24) & 0x3u) == 0x0u &&
      ((Word >> 26) & 1u) == 0u)
    return decodeLdStImm9OrReg(Word, Addr, Insn, Detail);

  if (((Word >> 27) & 0x7u) == 0x7u && ((Word >> 24) & 0x3u) == 0x0u &&
      ((Word >> 26) & 1u) == 1u)
    return decodeLdStImm9OrRegFP(Word, Addr, Insn, Detail);

  if (((Word >> 27) & 0x7u) == 0x5u && ((Word >> 26) & 1u) == 0u)
    return decodeLdStPair(Word, Addr, Insn, Detail);

  if (((Word >> 27) & 0x7u) == 0x5u && ((Word >> 26) & 1u) == 1u)
    return decodeLdStPairFP(Word, Addr, Insn, Detail);

  if (((Word >> 24) & 0x7Fu) == 0x1Eu && ((Word >> 21) & 1u) == 1u)
    return decodeScalarFP(Word, Addr, Insn, Detail);

  return decodeControlFlow(Word, Addr, Insn, Detail);
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEDISPATCH_H
