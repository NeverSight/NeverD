//===- BranchEncoding.h - Encodings of direct branches ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The rows of BranchEncoding.def as typed constants: the instruction forms,
/// the fields and the other constants of the direct branches and linker
/// thunks NeverD decodes, which ISAEncoding.h's constants of the same
/// instructions are defined from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BRANCHENCODING_H
#define NEVERD_SUPPORT_BRANCHENCODING_H

#include "neverd/support/InstructionFields.h"

#include "llvm/Support/MathExtras.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd {
namespace branch {

#define NEVERD_BRANCH_FORM(Name, Mask, Match)                                  \
  inline constexpr InstructionForm Name{Mask, Match};
#define NEVERD_BRANCH_FIELD(Name, Low, Width)                                  \
  inline constexpr BitField Name{Low, Width};
#define NEVERD_BRANCH_VALUE(Name, Value) inline constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

/// How far an A64 `b` or `bl` \p Word branches, in bytes from itself.
inline int64_t a64BranchDisplacement(uint32_t Word) {
  return llvm::SignExtend64(A64BranchOffset.extract(Word),
                            A64BranchOffset.Width) *
         A64InstructionBytes;
}

/// How far an A64 conditional branch \p Word -- B.cond, BC.cond, CBZ, CBNZ,
/// TBZ or TBNZ -- branches, in bytes from itself, or std::nullopt when
/// \p Word is no conditional branch.
inline std::optional<int64_t> a64ConditionalBranchDisplacement(uint32_t Word) {
  const bool TestsABit = A64TestBranch.matches(Word);
  if (!TestsABit && !A64ConditionalBranch.matches(Word) &&
      !A64CompareBranch.matches(Word))
    return std::nullopt;
  const BitField Offset =
      TestsABit ? A64TestBranchOffset : A64ConditionalBranchOffset;
  return llvm::SignExtend64(Offset.extract(Word), Offset.Width) *
         A64InstructionBytes;
}

/// \p Word, an A64 conditional branch, with its offset field set to branch
/// \p Displacement bytes, or std::nullopt when \p Word is no conditional
/// branch or its field cannot hold that displacement: a whole number of
/// instructions within the reach of the 19-bit field, or of the 14-bit one of
/// TBZ and TBNZ.
inline std::optional<uint32_t>
withA64ConditionalBranchDisplacement(uint32_t Word, int64_t Displacement) {
  const bool TestsABit = A64TestBranch.matches(Word);
  if (!TestsABit && !A64ConditionalBranch.matches(Word) &&
      !A64CompareBranch.matches(Word))
    return std::nullopt;
  constexpr unsigned InstructionBits =
      llvm::ConstantLog2<A64InstructionBytes>();
  const BitField Offset =
      TestsABit ? A64TestBranchOffset : A64ConditionalBranchOffset;
  if (Displacement % A64InstructionBytes != 0 ||
      !llvm::isIntN(Offset.Width + InstructionBits, Displacement))
    return std::nullopt;
  const uint32_t Instructions = static_cast<uint32_t>(
      static_cast<uint64_t>(Displacement >> InstructionBits));
  return (Word & ~Offset.mask()) |
         ((Instructions << Offset.Low) & Offset.mask());
}

/// Where an A64 `b` or `bl` \p Word at \p Address branches, or std::nullopt
/// when that lies outside the 64-bit address space.
inline std::optional<uint64_t> a64BranchTarget(uint32_t Word,
                                               uint64_t Address) {
  const int64_t Displacement = a64BranchDisplacement(Word);
  const uint64_t Magnitude = Displacement < 0
                                 ? 0 - static_cast<uint64_t>(Displacement)
                                 : static_cast<uint64_t>(Displacement);
  if (Displacement < 0 ? Address < Magnitude : Address > ~Magnitude)
    return std::nullopt;
  return Address + static_cast<uint64_t>(Displacement);
}

/// Whether an A64 `b` or `bl` can branch \p Displacement bytes: a whole
/// number of instructions its offset field holds.
inline bool isA64BranchDisplacement(int64_t Displacement) {
  constexpr unsigned InstructionBits =
      llvm::ConstantLog2<A64InstructionBytes>();
  return Displacement % A64InstructionBytes == 0 &&
         llvm::isIntN(A64BranchOffset.Width + InstructionBits, Displacement);
}

/// \p Word with its A64 `b` or `bl` offset field set to branch
/// \p Displacement bytes.  The bits below a whole instruction, and those the
/// field cannot hold, are cut off; isA64BranchDisplacement says whether any
/// were.
inline uint32_t withA64BranchDisplacement(uint32_t Word, int64_t Displacement) {
  constexpr unsigned InstructionBits =
      llvm::ConstantLog2<A64InstructionBytes>();
  const uint32_t Offset = static_cast<uint32_t>(
      static_cast<uint64_t>(Displacement >> InstructionBits));
  return (Word & ~A64BranchOffset.mask()) |
         ((Offset << A64BranchOffset.Low) & A64BranchOffset.mask());
}

/// An A32 `blx` (immediate) that branches \p Displacement bytes from its
/// program counter, which reads ArmPCOffset bytes past it.  \p Displacement
/// must be even and within the reach of an A32 `bl`.
inline uint32_t a32BranchLinkExchange(int64_t Displacement) {
  constexpr unsigned WordBits = llvm::ConstantLog2<ArmInstructionBytes>();
  constexpr unsigned HalfwordBits = llvm::ConstantLog2<ThumbHalfwordBytes>();
  const uint64_t Bytes = static_cast<uint64_t>(Displacement);
  return (ArmConditionUnconditional << ArmCondition.Low) | ArmBranch.Match |
         (static_cast<uint32_t>(Bytes >> HalfwordBits & 1)
          << ArmLinkOrHalfword.Low) |
         (static_cast<uint32_t>(Bytes >> WordBits) & ArmBranchOffset.mask());
}

namespace t32 {
/// Where the fields of a T32 `b.w`, `bl` or `blx` sit in the offset it
/// branches, S:I1:I2:imm10:imm11:0 in bytes.
inline constexpr unsigned LowOffset = llvm::ConstantLog2<ThumbHalfwordBytes>();
inline constexpr unsigned HighOffset = LowOffset + ThumbBranchLow.Width;
inline constexpr unsigned I2Bit = HighOffset + ThumbBranchHigh.Width;
inline constexpr unsigned I1Bit = I2Bit + ThumbBranchJ2.Width;
inline constexpr unsigned SignBit = I1Bit + ThumbBranchJ1.Width;
} // namespace t32

/// A same-mode T32 B.W. Its signed halfword displacement is independent of
/// the loaded image base and requires no absolute pointer in original text.
inline std::pair<uint16_t, uint16_t> t32Jump(int64_t Displacement) {
  const uint32_t Bytes = static_cast<uint32_t>(Displacement);
  const uint32_t S = (Bytes >> t32::SignBit) & 1;
  const uint32_t J1 = ~((Bytes >> t32::I1Bit) ^ S) & 1;
  const uint32_t J2 = ~((Bytes >> t32::I2Bit) ^ S) & 1;
  return {uint16_t(ThumbWideBranchHigh.Match | (S << ThumbBranchSign.Low) |
                   ((Bytes >> t32::HighOffset) & ThumbBranchHigh.mask())),
          uint16_t(ThumbJumpLow.Match | (J1 << ThumbBranchJ1.Low) |
                   (J2 << ThumbBranchJ2.Low) |
                   ((Bytes >> t32::LowOffset) & ThumbBranchLow.mask()))};
}

/// Whether a T32 `blx` (immediate) can branch \p Displacement bytes: a whole
/// number of words its offset holds.
inline bool isT32BranchLinkExchangeDisplacement(int64_t Displacement) {
  return Displacement % ArmInstructionBytes == 0 &&
         llvm::isIntN(t32::SignBit + ThumbBranchSign.Width, Displacement);
}

/// The two halfwords of a T32 `blx` (immediate) that branches \p Displacement
/// bytes from its program counter, which reads ThumbPCOffset bytes past it,
/// rounded down to a word.  isT32BranchLinkExchangeDisplacement must hold.
/// Its J1 and J2 are NOT(I1 XOR S) and NOT(I2 XOR S), and its imm11 is
/// imm10L:H with H clear.
inline std::pair<uint16_t, uint16_t>
t32BranchLinkExchange(int64_t Displacement) {
  const uint32_t Bytes = static_cast<uint32_t>(Displacement);
  const uint32_t S = Bytes >> t32::SignBit & 1;
  const uint32_t J1 = ~((Bytes >> t32::I1Bit) ^ S) & 1;
  const uint32_t J2 = ~((Bytes >> t32::I2Bit) ^ S) & 1;
  const uint32_t High = ThumbWideBranchHigh.Match | S << ThumbBranchSign.Low |
                        (Bytes >> t32::HighOffset & ThumbBranchHigh.mask());
  const uint32_t Low = ThumbLinkExchangeLow.Match | J1 << ThumbBranchJ1.Low |
                       J2 << ThumbBranchJ2.Low |
                       // imm11 but the H bit the BLX form fixes.
                       (Bytes >> t32::LowOffset & ThumbBranchLow.mask() &
                        ~ThumbLinkExchangeLow.Mask);
  return {static_cast<uint16_t>(High), static_cast<uint16_t>(Low)};
}

} // namespace branch
} // namespace neverd

#endif // NEVERD_SUPPORT_BRANCHENCODING_H
