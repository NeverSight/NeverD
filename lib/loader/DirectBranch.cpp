//===- DirectBranch.cpp - Direct branch scanning for runtime edges --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/DirectBranch.h"

#include "neverd/Support/BinaryEncoding.h"

namespace neverd {
namespace {

/// Decode the 32-bit Thumb-2 branch pair at \p Code.
///
/// `BL`, `B.W`, and `BLX` share one encoding split across two halfwords: the
/// first carries the sign and the high immediate bits, the second the J1/J2
/// pair that — exclusive-ored back against the sign — restores the two
/// immediate bits above them.  Nothing else distinguishes the three but two
/// bits of the second halfword, so they are decoded together.
///
/// Conditional `B.W` (T3) is deliberately not decoded.  It shares the leading
/// halfword pattern but names a branch within the function rather than a call
/// edge, exactly as the ARM-state decoder excludes a predicated `B`.
std::optional<va_t> decodeThumbBranch(const uint8_t *Code, size_t Available,
                                      va_t VA, size_t &Length) {
  if (Available < 4)
    return std::nullopt;
  const uint16_t Hw1 = readLE<uint16_t>(Code);
  const uint16_t Hw2 = readLE<uint16_t>(Code + 2);
  if ((Hw1 & 0xF800u) != 0xF000u || (Hw2 & 0x8000u) == 0)
    return std::nullopt;

  const bool IsLink = (Hw2 & 0x4000u) != 0;  // second halfword `11` vs `10`
  const bool WideImm = (Hw2 & 0x1000u) != 0; // BL/B.W vs BLX
  // `10x0` is conditional B.W (T3); `11x0` is BLX, which is a call edge.
  if (!IsLink && !WideImm)
    return std::nullopt;

  const uint32_t S = (Hw1 >> 10) & 1u;
  const uint32_t J1 = (Hw2 >> 13) & 1u;
  const uint32_t J2 = (Hw2 >> 11) & 1u;
  const uint32_t I1 = (~(J1 ^ S)) & 1u;
  const uint32_t I2 = (~(J2 ^ S)) & 1u;
  const uint32_t ImmHigh = Hw1 & 0x03FFu;
  const uint32_t ImmLow = Hw2 & 0x07FFu;

  Length = 4;
  // The Thumb program counter reads four bytes ahead of the instruction.  BLX
  // additionally rounds it down, because it targets ARM state where
  // instructions are word-aligned.
  const va_t Base = WideImm ? VA + 4 : ((VA + 4) & ~va_t(3));
  // BLX encodes a halfword-pair target, so its lowest immediate bit is the
  // reserved H bit rather than part of the displacement.
  const uint32_t Imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                         (ImmHigh << 12) |
                         ((WideImm ? ImmLow : (ImmLow & 0x07FEu)) << 1);
  // Bit 24 is the sign: shifting it up to bit 63 and back sign-extends it.
  const int64_t Offset = static_cast<int64_t>(Imm25) << 39 >> 39;
  if (Offset < 0 && static_cast<va_t>(-Offset) > Base)
    return std::nullopt;
  return static_cast<va_t>(static_cast<int64_t>(Base) + Offset);
}

} // namespace

std::optional<va_t> decodeDirectBranchTarget(Arch A, InstructionMode Mode,
                                             const uint8_t *Code,
                                             size_t Available, va_t VA,
                                             size_t &Length) {
  switch (A) {
  case Arch::X64:
  case Arch::X86: {
    if (Available < 5 || (Code[0] != 0xE8 && Code[0] != 0xE9))
      return std::nullopt;
    Length = 5;
    const int32_t Displacement = readLE<int32_t>(Code + 1);
    const va_t Next = VA + 5;
    if (Displacement < 0 && static_cast<va_t>(-int64_t(Displacement)) > Next)
      return std::nullopt;
    return static_cast<va_t>(static_cast<int64_t>(Next) + Displacement);
  }
  case Arch::AArch64: {
    if (Available < 4)
      return std::nullopt;
    Length = 4;
    const uint32_t Word = readLE<uint32_t>(Code);
    if ((Word & 0x7C000000u) != 0x14000000u)
      return std::nullopt; // neither B nor BL
    int64_t Offset = static_cast<int64_t>(Word & 0x03FFFFFFu) << 38 >> 36;
    if (Offset < 0 && static_cast<va_t>(-Offset) > VA)
      return std::nullopt;
    return static_cast<va_t>(static_cast<int64_t>(VA) + Offset);
  }
  case Arch::ARM: {
    if (Mode == InstructionMode::Thumb)
      return decodeThumbBranch(Code, Available, VA, Length);
    if (Available < 4)
      return std::nullopt;
    Length = 4;
    const uint32_t Word = readLE<uint32_t>(Code);
    // The A1 encodings of B and BL.  A condition other than 0b1110 is a
    // conditional branch, which does not name a call edge on its own.
    if ((Word >> 28) != 0xE || (Word & 0x0E000000u) != 0x0A000000u)
      return std::nullopt;
    int64_t Offset = static_cast<int64_t>(Word & 0x00FFFFFFu) << 40 >> 38;
    const va_t Next = VA + 8; // ARM pipeline offset
    if (Offset < 0 && static_cast<va_t>(-Offset) > Next)
      return std::nullopt;
    return static_cast<va_t>(static_cast<int64_t>(Next) + Offset);
  }
  default:
    return std::nullopt;
  }
}

} // namespace neverd
