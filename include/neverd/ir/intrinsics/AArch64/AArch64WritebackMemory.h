//===- AArch64WritebackMemory.h - Original writeback contracts --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Single GPR loads pack the completed X value below the base. Pair and SIMD
/// loads publish complete values and a base through typed auxiliary results;
/// stores return the base. No output precedes the original instruction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64WRITEBACKMEMORY_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64WRITEBACKMEMORY_H

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace neverd {

#define NEVERD_A64_WRITEBACK_VALUE(Type, Name, Value)                          \
  inline constexpr Type Name = Value;
#define NEVERD_A64_WRITEBACK_TEXT(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"

struct AArch64WritebackMemoryMachine {
  bool StackBase;
  bool Vector;
  bool Pair;
  bool Load;
  const char *Asm;
  const char *Constraints;
};

inline constexpr AArch64WritebackMemoryMachine
    AArch64WritebackMemoryMachines[] = {
#define NEVERD_A64_WRITEBACK_MACHINE(Stack, Vector, Pair, Load, Asm,           \
                                     Constraints)                              \
  {Stack, Vector, Pair, Load, Asm, Constraints},
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
};

inline bool isAArch64PairMemoryIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::A64_PairLoad || Id == Intrinsic::A64_PairStore ||
         Id == Intrinsic::A64_VectorPairLoad ||
         Id == Intrinsic::A64_VectorPairStore;
}

inline bool isAArch64WritebackMemoryIntrinsic(Intrinsic Id) {
  return aarch64StructureOperation(Id).has_value() ||
         Id == Intrinsic::A64_LoadWriteback ||
         Id == Intrinsic::A64_StoreWriteback ||
         Id == Intrinsic::A64_VectorLoadWriteback ||
         Id == Intrinsic::A64_VectorStoreWriteback ||
         isAArch64PairMemoryIntrinsic(Id);
}

inline bool isAArch64WritebackStoreIntrinsic(Intrinsic Id) {
  if (const auto Op = aarch64StructureOperation(Id))
    return !Op->Load;
  return Id == Intrinsic::A64_StoreWriteback ||
         Id == Intrinsic::A64_VectorStoreWriteback ||
         Id == Intrinsic::A64_PairStore || Id == Intrinsic::A64_VectorPairStore;
}

inline bool isAArch64WritebackAuxiliaryLoad(Intrinsic Id) {
  if (const auto Op = aarch64StructureOperation(Id))
    return Op->Load;
  return Id == Intrinsic::A64_VectorLoadWriteback ||
         Id == Intrinsic::A64_PairLoad || Id == Intrinsic::A64_VectorPairLoad;
}

inline unsigned aarch64WritebackBaseIndex(Intrinsic Id) {
  if (const auto Op = aarch64StructureOperation(Id))
    return Op->Vectors;
  return isAArch64PairMemoryIntrinsic(Id) ? kPairBaseIndex
                                          : kWritebackBaseIndex;
}

inline unsigned aarch64WritebackMemoryOperandCount(Intrinsic Id) {
  if (const auto Op = aarch64StructureOperation(Id))
    return kStructureDataOperand + (Op->Load && !Op->Lane ? 0 : Op->Vectors);
  if (!isAArch64WritebackStoreIntrinsic(Id))
    return kWritebackLoadOperands;
  return isAArch64PairMemoryIntrinsic(Id) ? kPairStoreOperands
                                          : kWritebackStoreOperands;
}

struct AArch64WritebackMemoryOperation {
  bool Load;
  unsigned AccessBytes;
  unsigned ResultBytes;
  bool Vector = false;
  bool Pair = false;
  bool Signed = false;
};

inline std::optional<AArch64WritebackMemoryOperation>
aarch64WritebackMemoryOperation(uint64_t Word) {
  if (Word > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  if (const auto Structure = aarch64StructureMemoryInstruction(Word))
    return AArch64WritebackMemoryOperation{
        Structure->isLoad(), Structure->ElementBytes,
        Structure->isLoad() ? kAddressMemoryVectorBytes : 0, true};
  switch (Word & kPairOpcodeMask) {
#define NEVERD_A64_PAIR_ENCODING(Encoding, Load, Access, Vector, Signed)       \
  case Encoding:                                                               \
    if (Signed && !(Word & kPairModeMask))                                     \
      return std::nullopt;                                                     \
    return AArch64WritebackMemoryOperation{                                    \
        Load,                                                                  \
        Access,                                                                \
        Load ? (Vector ? kAddressMemoryVectorBytes                             \
                       : kAddressMemoryPointerBytes)                           \
             : 0,                                                              \
        Vector,                                                                \
        true,                                                                  \
        Signed};
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
  default:
    break;
  }
  switch (Word & kWritebackOpcodeMask) {
#define NEVERD_A64_WRITEBACK_ENCODING(Encoding, Load, Access, Result)          \
  case Encoding:                                                               \
    return AArch64WritebackMemoryOperation{Load, Access, Result};
#define NEVERD_A64_WRITEBACK_VECTOR_ENCODING(Encoding, Load, Access)           \
  case Encoding:                                                               \
    return AArch64WritebackMemoryOperation{                                    \
        Load, Access, Load ? kAddressMemoryVectorBytes : 0, true};
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
  default:
    return std::nullopt;
  }
}

struct AArch64WritebackMemoryInstruction {
  uint32_t Word;

  auto operation() const { return *aarch64WritebackMemoryOperation(Word); }
  bool isLoad() const { return operation().Load; }
  bool isVector() const { return operation().Vector; }
  bool isPair() const { return operation().Pair; }
  bool isStructure() const { return isAArch64StructureMemoryEncoding(Word); }
  auto structure() const { return *aarch64StructureMemoryInstruction(Word); }
  Intrinsic id() const {
    if (isStructure())
      return structure().id();
    if (isPair())
      return isVector() ? (isLoad() ? Intrinsic::A64_VectorPairLoad
                                    : Intrinsic::A64_VectorPairStore)
                        : (isLoad() ? Intrinsic::A64_PairLoad
                                    : Intrinsic::A64_PairStore);
    if (isVector())
      return isLoad() ? Intrinsic::A64_VectorLoadWriteback
                      : Intrinsic::A64_VectorStoreWriteback;
    return isLoad() ? Intrinsic::A64_LoadWriteback
                    : Intrinsic::A64_StoreWriteback;
  }
  unsigned accessBytes() const { return operation().AccessBytes; }
  unsigned destination() const { return Word & kAddressMemoryRegisterMask; }
  unsigned secondDestination() const {
    return (Word >> kPairSecondShift) & kAddressMemoryRegisterMask;
  }
  unsigned base() const {
    return (Word >> kAddressMemoryBaseShift) & kAddressMemoryRegisterMask;
  }
  bool stackBase() const { return base() == kAddressMemoryRegisterMask; }
  const char *sourceBaseFailure(bool HasImage, bool HasRawSP) const {
    return aarch64MemorySourceBaseFailure(stackBase(), HasImage, HasRawSP);
  }
  const AArch64WritebackMemoryMachine &machine() const {
    return *std::find_if(std::begin(AArch64WritebackMemoryMachines),
                         std::end(AArch64WritebackMemoryMachines),
                         [&](const auto &M) {
                           return M.StackBase == stackBase() &&
                                  M.Vector == isVector() &&
                                  M.Pair == isPair() && M.Load == isLoad();
                         });
  }
  bool hasRegisterIndex() const {
    return isStructure() && structure().hasRegisterIndex();
  }
  bool hasWriteback() const {
    return isStructure() ? structure().hasWriteback()
                         : (!isPair() || (Word & kPairWritebackBit));
  }
  bool preIndex() const {
    if (isStructure())
      return false;
    return isPair() ? bool(Word & kPairPreIndexBit)
                    : bool(Word & kWritebackPreIndexBit);
  }
  bool offsetAccess() const {
    return !isStructure() && (!hasWriteback() || preIndex());
  }
  unsigned totalAccessBytes() const {
    return isStructure() ? structure().totalAccessBytes()
                         : accessBytes() * (isPair() ? kPairValues : 1);
  }
  AArch64AddressMemoryInstruction adjustment() const {
    return {Word & ~kWritebackModeMask};
  }
  uint64_t displacement() const {
    if (isStructure())
      return structure().hasWriteback() ? totalAccessBytes() : 0;
    if (!isPair())
      return adjustment().addressOffset(0);
    uint64_t Immediate = (Word >> kPairImmediateShift) & kPairImmediateMask;
    if (Immediate & kPairImmediateSignBit)
      Immediate |= ~uint64_t(kPairImmediateMask);
    return Immediate * accessBytes();
  }
  std::optional<uint64_t> completedBase(uint64_t Base,
                                        uint64_t Index = 0) const {
    if (stackBase())
      return std::nullopt;
    return aarch64ControlIndependentAddress(
        Base,
        hasWriteback() ? (hasRegisterIndex() ? Index : displacement()) : 0, 1);
  }
  std::optional<uint64_t> accessAddress(uint64_t Base) const {
    if (stackBase())
      return std::nullopt;
    return aarch64ControlIndependentAddress(
        Base, offsetAccess() ? displacement() : 0, totalAccessBytes());
  }

  /// This is a finite storage proof only. Execution always obtains the base
  /// from the machine instruction. A post-index access need not dereference
  /// its outgoing base, but that base must remain within the materialization.
  std::optional<AArch64AddressMemorySpan>
  controlIndependentSpan(uint64_t Base, uint64_t Index) const {
    if (!aarch64WritebackMemoryOperation(Word) ||
        (!hasRegisterIndex() && Index))
      return std::nullopt;
    const auto Updated = completedBase(Base, Index);
    const auto Address = accessAddress(Base);
    if (!Updated || !Address)
      return std::nullopt;
    if (*Address > std::numeric_limits<uint64_t>::max() - totalAccessBytes())
      return std::nullopt;
    constexpr uint64_t Region =
        kAddressMemoryPreservedMask | kAddressMemoryPoisonBit;
    if (((Base ^ *Address) | (Base ^ (*Address + totalAccessBytes() - 1)) |
         (Base ^ *Updated)) &
        Region)
      return std::nullopt;
    const auto Begin = std::min({Base, *Address, *Updated});
    const auto End = std::max({Base, *Address + totalAccessBytes(), *Updated});
    return AArch64AddressMemorySpan{Begin, End - Begin};
  }
  uint32_t scratchWord() const {
    if (isStructure())
      return structure().scratchWord();
    const uint32_t SecondMask =
        isPair() ? kAddressMemoryRegisterMask << kPairSecondShift : 0;
    return (Word & ~(kAddressMemoryBaseDestinationMask | SecondMask)) |
           (isPair() ? kPairSecondScratch << kPairSecondShift : 0) |
           kAddressMemoryScratchValue |
           ((stackBase() ? kAddressMemoryRegisterMask
                         : kAddressMemoryScratchBase)
            << kAddressMemoryBaseShift);
  }
};

inline std::optional<AArch64WritebackMemoryInstruction>
aarch64WritebackMemoryInstruction(uint64_t Word) {
  if (!aarch64WritebackMemoryOperation(Word))
    return std::nullopt;
  AArch64WritebackMemoryInstruction Instruction{static_cast<uint32_t>(Word)};
  // Both loads and stores have constrained-unpredictable writeback overlap.
  // Choosing different scratch registers cannot resolve that ambiguity.
  if (Instruction.isPair() && Instruction.isLoad() &&
      Instruction.destination() == Instruction.secondDestination())
    return std::nullopt;
  if (!Instruction.isVector() && Instruction.hasWriteback() &&
      !Instruction.stackBase() &&
      (Instruction.destination() == Instruction.base() ||
       (Instruction.isPair() &&
        Instruction.secondDestination() == Instruction.base())))
    return std::nullopt;
  return Instruction;
}

inline bool aarch64WritebackMemoryShapeIsValid(
    Intrinsic Id, unsigned NumInputs, unsigned OutputBytes, unsigned BaseBytes,
    unsigned WordBytes, std::optional<uint64_t> Word, unsigned DataBytes = 0,
    std::optional<uint64_t> Data = std::nullopt, unsigned SecondBytes = 0,
    std::optional<uint64_t> Second = std::nullopt) {
  if (aarch64StructureOperation(Id) || !isAArch64WritebackMemoryIntrinsic(Id) ||
      !Word || NumInputs != aarch64WritebackMemoryOperandCount(Id) + 1 ||
      BaseBytes != kAddressMemoryPointerBytes ||
      WordBytes != kAddressMemoryWordBytes)
    return false;
  const auto Instruction = aarch64WritebackMemoryInstruction(*Word);
  if (!Instruction || Instruction->id() != Id)
    return false;
  if (Instruction->isLoad())
    return OutputBytes == (isAArch64WritebackAuxiliaryLoad(Id)
                               ? 0
                               : kWritebackPairBytes) &&
           !DataBytes && !SecondBytes;
  if (Instruction->isPair() &&
      (SecondBytes != (Instruction->isVector() ? kAddressMemoryVectorBytes
                                               : kAddressMemoryPointerBytes) ||
       (!Instruction->isVector() &&
        Instruction->secondDestination() == kAddressMemoryRegisterMask &&
        (!Second || *Second))))
    return false;
  if (!Instruction->isPair() && SecondBytes)
    return false;
  if (Instruction->isVector())
    return OutputBytes == kAddressMemoryPointerBytes &&
           DataBytes == kAddressMemoryVectorBytes;
  return OutputBytes == kAddressMemoryPointerBytes &&
         DataBytes == kAddressMemoryPointerBytes &&
         (Instruction->destination() != kAddressMemoryRegisterMask ||
          (Data && !*Data));
}

inline constexpr uint8_t aarch64WritebackMemoryOutputCount(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_WRITEBACK_MULTI_OUTPUT(Operation, Count)                    \
  case Intrinsic::Operation:                                                   \
    return Count;
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
  default:
    return 0;
  }
}

/// The primary value/input shape is shared with LowIR; these typed auxiliary
/// values become explicit SSA definitions in MedIR and HighIR.
template <typename Outputs, typename Size>
inline bool aarch64WritebackMemoryOutputsAreValid(Intrinsic Id,
                                                  const Outputs &Values,
                                                  Size size) {
  if (Values.size() != aarch64WritebackMemoryOutputCount(Id))
    return false;
  if (!isAArch64WritebackAuxiliaryLoad(Id))
    return true;
  const unsigned ValueBytes = Id == Intrinsic::A64_PairLoad
                                  ? kAddressMemoryPointerBytes
                                  : kAddressMemoryVectorBytes;
  const unsigned BaseIndex = aarch64WritebackBaseIndex(Id);
  for (unsigned I = 0; I != BaseIndex; ++I)
    if (size(Values[I]) != ValueBytes)
      return false;
  return size(Values[BaseIndex]) == kAddressMemoryPointerBytes;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64WRITEBACKMEMORY_H
