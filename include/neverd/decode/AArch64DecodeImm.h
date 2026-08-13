//===- AArch64DecodeImm.h - AArch64 immediate field expansion ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Expansion and validation of the encoded immediate forms the AArch64
/// instruction set uses (the 8-bit FP immediate, the MoveWidePreferred
/// predicate, and the logical bitmask immediate).  Each reproduces the value
/// Capstone reports for the same encoding, and rejects exactly the encodings
/// Capstone rejects, so the classes built on them stay a strict subset.
/// Include neverd/decode/AArch64NativeDecode.h instead of this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64DECODEIMM_H
#define NEVERD_DECODE_AARCH64DECODEIMM_H

#include <bit>
#include <cstdint>
#include <cstring>

namespace neverd {
namespace a64native {

/// Expand an 8-bit AArch64 FP immediate to the real value it denotes, returned
/// as a double.  The 8-bit format encodes the same real number for every
/// precision (H/S/D), and all such values are exactly representable in double,
/// so the width-64 expansion equals the value Capstone reports in operands[].fp
/// for the H/S/D forms alike (VFPExpandImm with N=64).
inline double vfpExpandImmToDouble(unsigned Imm8) {
  const uint64_t Sign = (Imm8 >> 7) & 1u;
  const uint64_t B = (Imm8 >> 6) & 1u;
  // exp[10]=NOT(b); exp[9:2]=b replicated 8x; exp[1:0]=imm8<5:4>.
  const uint64_t Exp =
      ((~B & 1ULL) << 10) | ((B ? 0xFFULL : 0ULL) << 2) | ((Imm8 >> 4) & 3ULL);
  const uint64_t Frac = static_cast<uint64_t>(Imm8 & 0xFu) << 48;
  const uint64_t Bits = (Sign << 63) | (Exp << 52) | Frac;
  double D;
  std::memcpy(&D, &Bits, 8);
  return D;
}

/// Would \p Value be produced by some MOVZ/MOVN 16-bit wide-immediate move?
/// This is the ARM \c MoveWidePreferred predicate (mirrored from LLVM's
/// AArch64AddressingModes): when true, Capstone keeps the 3-operand `orr Rd,
/// xzr, #imm` form; when false it renders the `mov Rd, #imm` (bitmask) alias.
inline bool isAnyMovwMovAlias(uint64_t Value, unsigned RegWidth) {
  auto isMovz = [](uint64_t V, unsigned Shift) {
    return V == (V & (0xFFFFULL << Shift));
  };
  auto isMovn = [RegWidth](uint64_t V, unsigned Shift) {
    uint64_t NotV = ~V;
    if (RegWidth == 32)
      NotV &= 0xFFFFFFFFULL;
    return NotV == (NotV & (0xFFFFULL << Shift));
  };
  for (unsigned Shift = 0; Shift + 16 <= RegWidth; Shift += 16)
    if (isMovz(Value, Shift) || isMovn(Value, Shift))
      return true;
  return false;
}

/// Decode an AArch64 logical bitmask immediate (N:immr:imms) to its 64/32-bit
/// value, returning false for the UNDEFINED encodings Capstone rejects, so the
/// class stays a strict subset.  This mirrors the architectural DecodeBitMasks
/// with immediate=TRUE; Capstone reports exactly this value in operands[].imm.
inline bool decodeBitMasks(bool Sf, unsigned N, unsigned Imms, unsigned Immr,
                           uint64_t &Out) {
  const unsigned DataSize = Sf ? 64u : 32u;
  const unsigned V = (N << 6) | ((~Imms) & 0x3Fu); // immN:NOT(imms), 7 bits
  if (V == 0)
    return false; // len < 0
  const int Len = static_cast<int>(std::bit_width(V)) - 1;
  if (Len < 1)
    return false;
  const unsigned ESize = 1u << Len;
  if (ESize > DataSize)
    return false; // e.g. 32-bit form with N==1
  const unsigned Levels = ESize - 1u;
  const unsigned S = Imms & Levels;
  const unsigned R = Immr & Levels;
  if (S == Levels)
    return false; // imms all-ones within the field -> UNDEFINED
  const uint64_t ESizeMask = (ESize >= 64) ? ~0ULL : ((1ULL << ESize) - 1);
  uint64_t Welem = ((S + 1u) >= 64u) ? ~0ULL : ((1ULL << (S + 1u)) - 1);
  Welem &= ESizeMask;
  uint64_t Ror =
      (R == 0) ? Welem : (((Welem >> R) | (Welem << (ESize - R))) & ESizeMask);
  uint64_t Wmask = 0;
  for (unsigned I = 0; I < DataSize; I += ESize)
    Wmask |= Ror << I;
  if (DataSize == 32)
    Wmask &= 0xFFFFFFFFULL;
  Out = Wmask;
  return true;
}

} // namespace a64native
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64DECODEIMM_H
