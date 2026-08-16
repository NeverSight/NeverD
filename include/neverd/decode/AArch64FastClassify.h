//===- AArch64FastClassify.h - Native fixed-width AArch64 classify -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capstone-free classification of a single 32-bit AArch64 instruction word.
///
/// AArch64 is fixed 4-byte width and 4-byte aligned, so the front-end scans
/// that only need an instruction's control-flow class (direct-call/-branch
/// target, whether it terminates a straight-line decode) can classify a word
/// with a handful of mask+shift tests instead of driving Capstone's decode DFA.
/// A differential parity check over 21.4M real instructions plus an exhaustive
/// sweep of the distinguishing encoding fields shows these predicates match a
/// full Capstone decode bit-for-bit (see AArch64_FastClassifyParityTests), at
/// ~700x the throughput (memory-bandwidth bound rather than DFA bound).
///
/// The predicates are intentionally defined to match the *current* Capstone
/// instruction-id semantics used by AArch64Lifter, not the architectural ideal
/// — e.g. `b.cond` shares id AARCH64_INS_B with unconditional `b`, so both are
/// terminators here, while the ARMv8.8 `bc.cond` (a distinct id) is not.  This
/// keeps the fast path a drop-in for the Capstone-id path it replaces.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64FASTCLASSIFY_H
#define NEVERD_DECODE_AARCH64FASTCLASSIFY_H

#include "neverd/Common.h"

#include <cstdint>

namespace neverd {
namespace a64fast {

/// Sign-extend the 26-bit branch immediate of a B/BL word and scale it to a
/// byte offset (the imm26 field counts 4-byte words).
inline int64_t branchImm26Offset(uint32_t Word) {
  int64_t Imm = static_cast<int64_t>(Word & 0x03FFFFFFu);
  Imm = (Imm ^ 0x02000000) - 0x02000000; // sign-extend bit 25
  return Imm << 2;
}

/// Direct (immediate) call target of \p Word at \p Addr, or InvalidVA when the
/// word is not a `BL`.  Matches AArch64Lifter::directCallTarget exactly: BL is
/// the only AArch64 direct call (BLR is indirect).  BL: bits[31:26] == 0b100101
/// (0x25); target = PC + SignExtend(imm26:00).
inline va_t directCallTarget(uint32_t Word, va_t Addr) {
  if ((Word >> 26) != 0x25u)
    return InvalidVA;
  return static_cast<va_t>(Addr + static_cast<va_t>(branchImm26Offset(Word)));
}

/// Whether \p Word ends a function's straight-line decode, bit-exact to
/// AArch64Lifter::isFunctionTerminator applied to a Capstone decode of the same
/// word.  The Capstone-id terminator set is { RET, BR, ERET, B, BRK, HLT },
/// where:
///
///   * RET  — 0xD65F0000 | (Rn<<5); the PAC forms RETAA/RETAB (ids 888/890)
///            have bits[11:10] set and are excluded (not in the set).
///   * BR   — 0xD61F0000 | (Rn<<5); BRAA/BRAB set bit 11 and Capstone rejects
///            the bare-Xn masked form, so they are excluded.
///   * ERET — the single encoding 0xD69F03E0; ERETAA/ERETAB (315/316) differ.
///   * B (id 51) — BOTH unconditional `b` (bits[31:26]==0b000101) AND
///            conditional `b.cond` (bits[31:24]==0x54 with bit4==0).  The
///            ARMv8.8 `bc.cond` (bits[31:24]==0x54 with bit4==1) is id 53, a
///            distinct id NOT in the set, so it is excluded by the bit-4 test.
///   * BRK/HLT — exception-generation encodings with a 16-bit immediate in
///            bits[20:5]; both lower to a non-returning trap.
inline bool isTerminatorWord(uint32_t Word) {
  if ((Word & 0xFFFFFC1Fu) == 0xD65F0000u) // RET {Xn}
    return true;
  if ((Word & 0xFFFFFC1Fu) == 0xD61F0000u) // BR Xn
    return true;
  if (Word == 0xD69F03E0u) // ERET
    return true;
  if ((Word >> 26) == 0x05u) // B (unconditional, id 51)
    return true;
  if ((Word >> 24) == 0x54u && (Word & 0x10u) == 0u) // B.cond (id 51)
    return true;
  if ((Word & 0xFFE0001Fu) == 0xD4200000u) // BRK #imm16
    return true;
  if ((Word & 0xFFE0001Fu) == 0xD4400000u) // HLT #imm16
    return true;
  return false;
}

} // namespace a64fast
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64FASTCLASSIFY_H
