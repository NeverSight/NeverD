//===- AArch64AddressMemory.h - Original memory contracts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain the memory instruction, rather than evaluating its address with an
/// unchecked integer sum. No image subtype proves runtime SCTLR2 controls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>

namespace neverd {

#define NEVERD_A64_ADDRESS_MEMORY_VALUE(Type, Name, Value)                     \
  inline constexpr Type Name = Value;
#define NEVERD_A64_ADDRESS_MEMORY_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"

/// A raw SP parameter supplies its alignment/tag bits only without an image;
/// a relocated address or a synthetic frame is not that environment.
inline const char *aarch64MemorySourceBaseFailure(bool StackBase, bool HasImage,
                                                  bool HasRawSP) {
  return StackBase && (HasImage || !HasRawSP) ? kAddressMemorySPUnsupported
                                              : nullptr;
}

/// A sufficient finite proof, independent of CPTA and CPACHECK controls.
/// This grants no mapping, tagging, alignment or fault permission.
inline std::optional<uint64_t>
aarch64ControlIndependentAddress(uint64_t Base, uint64_t Offset,
                                 unsigned AccessBytes) {
  const uint64_t Address = Base + Offset;
  const bool PreviousDetection = bool(Base & kAddressMemorySignBit) !=
                                 bool(Base & kAddressMemoryPoisonBit);
  const bool Detected =
      ((Address ^ Base) & kAddressMemoryTagMask) || PreviousDetection;
  const uint64_t Checked =
      (Address & ~(kAddressMemoryPreservedMask | kAddressMemoryPoisonBit)) |
      (Base & kAddressMemoryPreservedMask) |
      (Base & kAddressMemorySignBit ? 0 : kAddressMemoryPoisonBit);
  if (Detected && Address != Checked)
    return std::nullopt;
  constexpr uint64_t Region =
      kAddressMemoryPreservedMask | kAddressMemoryPoisonBit;
  const uint64_t LastByte = AccessBytes - 1;
  if (Address > std::numeric_limits<uint64_t>::max() - LastByte ||
      ((Address ^ (Address + LastByte)) & Region))
    return std::nullopt;
  return Address;
}

struct AArch64AddressMemoryMachine {
  bool StackBase;
  bool Vector;
  bool Store;
  const char *Asm;
  const char *Constraints;
};

inline constexpr AArch64AddressMemoryMachine kAddressMemoryMachines[] = {
#define NEVERD_A64_ADDRESS_MEMORY_MACHINE(Stack, Vector, Store, Asm,           \
                                          Constraints)                         \
  {Stack, Vector, Store, Asm, Constraints},
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
};

inline bool isAArch64AddressMemoryIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_ADDRESS_MEMORY_OPERATION(Id, ...) case Intrinsic::Id:
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
    return true;
  default:
    return false;
  }
}

inline bool isAArch64AddressStoreIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_ADDRESS_MEMORY_OPERATION(Id, Mnemonic, Store, Vector)       \
  case Intrinsic::Id:                                                          \
    return Store;
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
  default:
    return false;
  }
}

inline bool isAArch64AddressVectorIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_ADDRESS_MEMORY_OPERATION(Id, Mnemonic, Store, Vector)       \
  case Intrinsic::Id:                                                          \
    return Vector;
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
  default:
    return false;
  }
}

inline unsigned aarch64AddressMemoryOperandCount(Intrinsic Id) {
  return isAArch64AddressStoreIntrinsic(Id) ? kAddressStoreOperandCount
                                            : kAddressLoadOperandCount;
}

enum class AArch64AddressMemoryMode {
  Register,
  UnsignedImmediate,
  UnscaledImmediate
};

struct AArch64AddressMemoryOperation {
  Intrinsic Id;
  unsigned AccessBytes;
  unsigned ResultBytes;
  bool Signed;
  AArch64AddressMemoryMode Mode = AArch64AddressMemoryMode::Register;
};

/// The complete half-open byte span, including the raw base used to rebase
/// the original instruction. This is address arithmetic evidence only.
struct AArch64AddressMemorySpan {
  uint64_t Begin;
  uint64_t Bytes;
};

inline std::optional<AArch64AddressMemoryOperation>
aarch64AddressMemoryOperation(uint64_t Word) {
  if (Word > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  switch (Word & kAddressMemoryOpcodeMask) {
#define NEVERD_A64_ADDRESS_MEMORY_ENCODING(Id, Word, Access, Result, Signed)   \
  case Word:                                                                   \
    return AArch64AddressMemoryOperation{Intrinsic::Id, Access, Result, Signed};
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
  default:
    break;
  }
  switch (Word & kAddressMemoryUnsignedOpcodeMask) {
#define NEVERD_A64_ADDRESS_UNSIGNED_MEMORY_ENCODING(Id, Word, Access, Result,  \
                                                    Signed)                    \
  case Word:                                                                   \
    return AArch64AddressMemoryOperation{                                      \
        Intrinsic::Id, Access, Result, Signed,                                 \
        AArch64AddressMemoryMode::UnsignedImmediate};
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
  default:
    break;
  }
  switch (Word & kAddressMemoryUnscaledOpcodeMask) {
#define NEVERD_A64_ADDRESS_UNSCALED_MEMORY_ENCODING(Id, Word, Access, Result,  \
                                                    Signed)                    \
  case Word:                                                                   \
    return AArch64AddressMemoryOperation{                                      \
        Intrinsic::Id, Access, Result, Signed,                                 \
        AArch64AddressMemoryMode::UnscaledImmediate};
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
  default:
    break;
  }
  return std::nullopt;
}

struct AArch64AddressMemoryInstruction {
  uint32_t Word;

  AArch64AddressMemoryOperation operation() const {
    return *aarch64AddressMemoryOperation(Word);
  }
  bool isLoad() const { return operation().ResultBytes != 0; }
  bool isVector() const {
    return isAArch64AddressVectorIntrinsic(operation().Id);
  }
  unsigned accessBytes() const { return operation().AccessBytes; }
  unsigned totalAccessBytes() const { return accessBytes(); }
  bool hasRegisterIndex() const {
    return operation().Mode == AArch64AddressMemoryMode::Register;
  }
  bool usesFullWidthIndex() const {
    return !hasRegisterIndex() || (Word & kAddressMemoryWideIndexBit);
  }
  unsigned indexBytes() const {
    return usesFullWidthIndex() ? kAddressMemoryPointerBytes
                                : kAddressMemoryNarrowIndexBytes;
  }
  uint64_t extendIndex(uint64_t Index) const {
    if (usesFullWidthIndex())
      return Index;
    const uint64_t Narrow = Index & kAddressMemoryNarrowMask;
    return (Word & kAddressMemorySignedIndexBit) &&
                   (Narrow & kAddressMemoryNarrowSignBit)
               ? Narrow | ~kAddressMemoryNarrowMask
               : Narrow;
  }
  unsigned destination() const { return Word & kAddressMemoryRegisterMask; }
  unsigned base() const {
    return (Word >> kAddressMemoryBaseShift) & kAddressMemoryRegisterMask;
  }
  unsigned index() const {
    return hasRegisterIndex()
               ? (Word >> kAddressMemoryIndexShift) & kAddressMemoryRegisterMask
               : kAddressMemoryRegisterMask;
  }
  bool stackBase() const { return base() == kAddressMemoryRegisterMask; }
  const char *sourceBaseFailure(bool HasImage, bool HasRawSP) const {
    return aarch64MemorySourceBaseFailure(stackBase(), HasImage, HasRawSP);
  }
  unsigned indexShift() const {
    return hasRegisterIndex() && (Word & kAddressMemoryScaleMask)
               ? std::countr_zero(accessBytes())
               : 0;
  }
  uint64_t addressOffset(uint64_t Index) const {
    switch (operation().Mode) {
    case AArch64AddressMemoryMode::Register:
      return extendIndex(Index) << indexShift();
    case AArch64AddressMemoryMode::UnsignedImmediate:
      return ((Word >> kAddressMemoryUnsignedOffsetShift) &
              kAddressMemoryUnsignedOffsetMask) *
             uint64_t(accessBytes());
    case AArch64AddressMemoryMode::UnscaledImmediate: {
      const uint64_t Offset = (Word >> kAddressMemoryUnscaledOffsetShift) &
                              kAddressMemoryUnscaledOffsetMask;
      return Offset & kAddressMemoryUnscaledOffsetSign
                 ? Offset | ~kAddressMemoryUnscaledOffsetMask
                 : Offset;
    }
    }
    return 0;
  }
  /// Return an address only when CPTA on/off and every CPACHECK choice for
  /// the entire access have identical bits. This grants no mapping, tagging,
  /// alignment or no-fault evidence; consumers still need their memory model.
  std::optional<uint64_t> controlIndependentAddress(uint64_t Base,
                                                    uint64_t Index) const {
    if (!aarch64AddressMemoryOperation(Word) || stackBase() ||
        (index() == kAddressMemoryRegisterMask && Index))
      return std::nullopt;
    return aarch64ControlIndependentAddress(Base, addressOffset(Index),
                                            accessBytes());
  }
  /// A sufficient proof for every nonnegative raw index in [0, MaximumIndex].
  /// Monotonic addition must remain in one preserved-address region through
  /// the final accessed byte. This grants no memory or selector-domain proof.
  bool controlIndependentAddressRange(uint64_t Base,
                                      uint64_t MaximumIndex) const {
    if (!aarch64AddressMemoryOperation(Word))
      return false;
    constexpr uint64_t Region =
        kAddressMemoryPreservedMask | kAddressMemoryPoisonBit;
    if (!hasRegisterIndex()) {
      const auto Address = controlIndependentAddress(Base, 0);
      return MaximumIndex == 0 && Address &&
             !(((Base ^ *Address) | (Base ^ (*Address + accessBytes() - 1))) &
               Region);
    }
    if ((index() == kAddressMemoryRegisterMask && MaximumIndex) ||
        (!usesFullWidthIndex() && MaximumIndex > kAddressMemoryNarrowMask) ||
        extendIndex(MaximumIndex) != MaximumIndex ||
        MaximumIndex > (std::numeric_limits<uint64_t>::max() >> indexShift()))
      return false;
    const uint64_t Offset = MaximumIndex << indexShift();
    const uint64_t LastByte = accessBytes() - 1;
    if (Base > std::numeric_limits<uint64_t>::max() - Offset ||
        Base + Offset > std::numeric_limits<uint64_t>::max() - LastByte)
      return false;
    const uint64_t End = Base + Offset + LastByte;
    return !((Base ^ End) & Region) &&
           controlIndependentAddress(Base, 0).has_value() &&
           controlIndependentAddress(Base, MaximumIndex).has_value();
  }
  std::optional<AArch64AddressMemorySpan>
  controlIndependentSpan(uint64_t Base, uint64_t MaximumIndex) const {
    if (!controlIndependentAddressRange(Base, MaximumIndex))
      return std::nullopt;
    const uint64_t First = *controlIndependentAddress(Base, 0);
    const uint64_t Last = *controlIndependentAddress(Base, MaximumIndex);
    if (Last > std::numeric_limits<uint64_t>::max() - accessBytes())
      return std::nullopt;
    const uint64_t Begin = First < Base ? First : Base;
    const uint64_t End =
        Last + accessBytes() > Base ? Last + accessBytes() : Base;
    return AArch64AddressMemorySpan{Begin, End - Begin};
  }
  uint32_t scratchWord() const {
    const unsigned Base =
        stackBase() ? kAddressMemoryRegisterMask : kAddressMemoryScratchBase;
    // Immediate fields belong to the original instruction, including bits
    // that the register-offset form uses for Rm or the extension option.
    if (!hasRegisterIndex())
      return (Word & ~kAddressMemoryBaseDestinationMask) |
             kAddressMemoryScratchValue | (Base << kAddressMemoryBaseShift);
    // Replacing XZR's index with a zero-valued GPR preserves its value. The
    // original shift stays encoded and is applied exactly once by the load.
    return (Word & ~kAddressMemoryRegistersMask) | kAddressMemoryScratchValue |
           (Base << kAddressMemoryBaseShift) |
           (kAddressMemoryScratchIndex << kAddressMemoryIndexShift);
  }
  const AArch64AddressMemoryMachine &machine() const {
    return *std::find_if(std::begin(kAddressMemoryMachines),
                         std::end(kAddressMemoryMachines), [&](const auto &M) {
                           return M.StackBase == stackBase() &&
                                  M.Vector == isVector() && M.Store != isLoad();
                         });
  }
};

inline std::optional<AArch64AddressMemoryInstruction>
aarch64AddressMemoryInstruction(uint64_t Word) {
  if (!aarch64AddressMemoryOperation(Word))
    return std::nullopt;
  return AArch64AddressMemoryInstruction{static_cast<uint32_t>(Word)};
}

/// Includes unallocated size/opcode combinations, which must not fall back to
/// a mnemonic-based legacy handler with guessed access semantics.
inline bool isAArch64OrderedUnscaledMemoryEncoding(uint32_t Word) {
  const uint32_t Class = Word & kAddressMemoryOrderedClassMask;
  return Class == kAddressMemoryOrderedGPRClass ||
         Class == kAddressMemoryOrderedVectorClass;
}

inline bool aarch64AddressMemoryShapeIsValid(
    Intrinsic Id, unsigned NumInputs, unsigned OutputBytes, unsigned BaseBytes,
    unsigned IndexBytes, unsigned WordBytes, std::optional<uint64_t> Word,
    std::optional<uint64_t> Index, unsigned DataBytes = 0,
    std::optional<uint64_t> Data = std::nullopt) {
  const auto Instruction =
      Word ? aarch64AddressMemoryInstruction(*Word) : std::nullopt;
  if (!Instruction || Id != Instruction->operation().Id ||
      NumInputs != aarch64AddressMemoryOperandCount(Id) + 1 ||
      OutputBytes != (Instruction->isLoad() ? Instruction->accessBytes() : 0) ||
      BaseBytes != kAddressMemoryPointerBytes ||
      IndexBytes != Instruction->indexBytes() ||
      WordBytes != kAddressMemoryWordBytes ||
      (Instruction->index() == kAddressMemoryRegisterMask &&
       (!Index || *Index)))
    return false;
  if (Instruction->isLoad())
    return DataBytes == 0;
  return DataBytes == Instruction->accessBytes() &&
         (Instruction->isVector() ||
          Instruction->destination() != kAddressMemoryRegisterMask ||
          (Data && !*Data));
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H
