//===- AArch64StructureMemory.h - NEON memory semantics --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One decoder owns baseline NEON structure layout and register roles.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64STRUCTUREMEMORY_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64STRUCTUREMEMORY_H

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"

#include "llvm/Support/ErrorHandling.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace neverd {
#define NEVERD_A64_STRUCTURE_VALUE(Type, Name, Value)                          \
  inline constexpr Type Name = Value;
#define NEVERD_A64_STRUCTURE_TEXT(Name, Value)                                 \
  inline constexpr char Name[] = Value;
#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.def"

struct AArch64StructureOperation {
  unsigned Vectors;
  bool Load;
  bool Lane;
};

inline std::optional<AArch64StructureOperation>
aarch64StructureOperation(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_STRUCTURE_OPERATION(Id, Count, Load, Lane)                  \
  case Intrinsic::Id:                                                          \
    return AArch64StructureOperation{Count, Load, Lane};
#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.def"
  default:
    return std::nullopt;
  }
}

inline bool isAArch64StructureMemoryEncoding(uint32_t Word) {
  return (Word & kStructureMultipleMask) == kStructureMultiple ||
         (Word & kStructureSingleMask) == kStructureSingle;
}

struct AArch64StructureMemoryInstruction {
  uint32_t Word;
  unsigned Members;
  unsigned Repetitions;
  unsigned ElementBytes;
  unsigned Elements;
  unsigned Lane;
  bool Replicate;
  bool Single;

  bool isLoad() const { return Word & kStructureLoadBit; }
  bool laneLoad() const { return isLoad() && Single && !Replicate; }
  unsigned vectors() const { return Members * Repetitions; }
  unsigned totalAccessBytes() const {
    return Repetitions * Members * Elements * ElementBytes;
  }
  unsigned vectorBytes() const {
    return Word & kStructureQBit ? kAddressMemoryVectorBytes
                                 : kStructureNarrowBytes;
  }
  unsigned base() const {
    return (Word >> kAddressMemoryBaseShift) & kAddressMemoryRegisterMask;
  }
  bool stackBase() const { return base() == kAddressMemoryRegisterMask; }
  unsigned destination() const { return Word & kAddressMemoryRegisterMask; }
  unsigned index() const {
    return (Word >> kStructureIndexShift) & kAddressMemoryRegisterMask;
  }
  bool hasWriteback() const { return Word & kStructurePostBit; }
  bool hasRegisterIndex() const {
    return hasWriteback() && index() != kAddressMemoryRegisterMask;
  }
  Intrinsic id() const {
#define NEVERD_A64_STRUCTURE_OPERATION(Id, Count, Load, Lane)                  \
  if (vectors() == Count && isLoad() == Load && laneLoad() == Lane)            \
    return Intrinsic::Id;
#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.def"
    llvm_unreachable(kStructureInvalidIdentity);
  }
  unsigned memoryVector(unsigned Access) const {
    return (Access / (Members * Elements)) * Members + Access % Members;
  }
  unsigned memoryVectorByte(unsigned Access) const {
    return (Lane + (Access / Members) % Elements) * ElementBytes;
  }
  uint32_t scratchWord() const {
    const uint32_t IndexMask = kAddressMemoryRegisterMask
                               << kStructureIndexShift;
    return (Word & ~(kAddressMemoryBaseDestinationMask | IndexMask)) |
           kStructureScratchVector |
           ((stackBase() ? kAddressMemoryRegisterMask
                         : kAddressMemoryScratchBase)
            << kAddressMemoryBaseShift) |
           ((hasRegisterIndex() ? kStructureScratchIndex : index())
            << kStructureIndexShift);
  }
};

inline std::optional<AArch64StructureMemoryInstruction>
aarch64StructureMemoryInstruction(uint64_t RawWord) {
  if (RawWord > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  const auto Word = static_cast<uint32_t>(RawWord);
  if (!isAArch64StructureMemoryEncoding(Word))
    return std::nullopt;
  const bool Wide = Word & kStructureQBit;
  const unsigned Size = (Word >> kStructureSizeShift) & kStructureSizeMask;
  AArch64StructureMemoryInstruction I{
      Word, 1, 1,     1U << Size,
      1,    0, false, (Word & kStructureSingleMask) == kStructureSingle};
  if (!I.hasWriteback() && I.index())
    return std::nullopt;
  if (!I.Single) {
    switch ((Word >> kStructureOpcodeShift) & kStructureOpcodeMask) {
#define NEVERD_A64_STRUCTURE_LAYOUT(Opcode, Count, Repeat)                     \
  case Opcode:                                                                 \
    I.Members = Count;                                                         \
    I.Repetitions = Repeat;                                                    \
    break;
#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.def"
    default:
      return std::nullopt;
    }
    if (!Wide && I.ElementBytes == kStructureNarrowBytes && I.Members != 1)
      return std::nullopt;
    I.Elements = I.vectorBytes() / I.ElementBytes;
  } else {
    const unsigned Opcode =
        (Word >> kStructureSingleOpcodeShift) & kStructureSingleOpcodeMask;
    I.Members = 1 + 2 * (Opcode & 1) + bool(Word & kStructureCountBit);
    const bool S = Word & kStructureLaneBit;
    switch (Opcode >> 1) {
    case kStructureByteScale:
      I.ElementBytes = 1;
      I.Lane =
          kStructureNarrowBytes * Wide + (kStructureNarrowBytes / 2) * S + Size;
      break;
    case kStructureHalfScale:
      if (Size & 1)
        return std::nullopt;
      I.ElementBytes = 2;
      I.Lane = (kStructureNarrowBytes * Wide + (kStructureNarrowBytes / 2) * S +
                Size) /
               I.ElementBytes;
      break;
    case kStructureWordScale:
      if (Size > 1 || (Size && S))
        return std::nullopt;
      I.ElementBytes = Size ? kStructureNarrowBytes : kAddressMemoryWordBytes;
      I.Lane = Size ? unsigned(Wide) : 2 * Wide + S;
      break;
    case kStructureReplicateScale:
      if (!I.isLoad() || S)
        return std::nullopt;
      I.Replicate = true;
      break;
    }
  }
  return I;
}

/// Validate scalar metadata and every independently typed vector at the IR
/// boundary. The index is an explicit zero when the encoding has no Xm.
template <typename Size, typename Constant>
inline bool aarch64StructureShapeIsValid(Intrinsic Id, unsigned Inputs,
                                         unsigned OutputBytes, Size size,
                                         Constant constant) {
  const auto Op = aarch64StructureOperation(Id);
  if (!Op ||
      Inputs !=
          kStructureDataOperand + (Op->Load && !Op->Lane ? 0 : Op->Vectors) ||
      OutputBytes != (Op->Load ? 0 : kAddressMemoryPointerBytes) ||
      size(0) != kAddressMemoryPointerBytes ||
      size(1) != kAddressMemoryWordBytes ||
      size(kStructureIndexOperand) != kAddressMemoryPointerBytes)
    return false;
  const auto Word = constant(1);
  const auto I = Word ? aarch64StructureMemoryInstruction(*Word) : std::nullopt;
  if (!I || I->id() != Id ||
      (!I->hasRegisterIndex() &&
       constant(kStructureIndexOperand) != std::optional<uint64_t>(0)))
    return false;
  for (unsigned N = kStructureDataOperand; N != Inputs; ++N)
    if (size(N) != kAddressMemoryVectorBytes)
      return false;
  return true;
}

} // namespace neverd
#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64STRUCTUREMEMORY_H
