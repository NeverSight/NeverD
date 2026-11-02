//===- AArch64PAuthLoad.h - Combined load contracts ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The original word owns key, offset, SP provenance and writeback. The low
/// half of the result is the loaded value; the high half is the outgoing base
/// (unchanged for offset addressing). Both become visible only after success.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64PAUTHLOAD_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64PAUTHLOAD_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace neverd {

#define NEVERD_A64_PAUTH_LOAD_VALUE(Type, Name, Value)                         \
  inline constexpr Type Name = Value;
#define NEVERD_A64_PAUTH_LOAD_TEXT(Name, Text)                                 \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.def"

inline bool isAArch64PAuthLoadIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_PAUTH_LOAD_OPERATION(Id, Mnemonic, Encoding)                \
  case Intrinsic::Id:
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.def"
    return true;
  default:
    return false;
  }
}

struct AArch64PAuthLoadInstruction {
  Intrinsic Id;
  uint32_t Word;

  unsigned destination() const { return Word & kPAuthLoadRegisterMask; }
  unsigned base() const {
    return (Word >> kPAuthLoadBaseShift) & kPAuthLoadRegisterMask;
  }
  bool stackBase() const { return base() == kPAuthLoadRegisterMask; }
  const char *sourceBaseFailure(bool HasImage, bool HasRawSP) const {
    return stackBase() && (HasImage || !HasRawSP) ? kPAuthLoadSPUnsupported
                                                  : nullptr;
  }
  const char *llvmAsm() const {
    return stackBase() ? kPAuthLoadSPAsm : kPAuthLoadAsm;
  }
  const char *llvmConstraints() const {
    return stackBase() ? kPAuthLoadSPConstraints : kPAuthLoadConstraints;
  }
  bool writeback() const { return Word & kPAuthLoadWritebackMask; }
  int64_t displacement() const {
    const bool Negative = (Word >> kPAuthLoadSignShift) & 1U;
    const int64_t Immediate =
        ((Word >> kPAuthLoadImmediateShift) & kPAuthLoadImmediateMask) |
        (uint32_t(Negative) << kPAuthLoadSignBit);
    return (Immediate - (Negative ? kPAuthLoadImmediateModulus : 0)) *
           kPAuthLoadPointerBytes;
  }
  uint32_t scratchWord() const {
    return (Word & ~kPAuthLoadRegistersMask) | kPAuthLoadScratchValue |
           ((stackBase() ? kPAuthLoadRegisterMask : kPAuthLoadScratchBase)
            << kPAuthLoadBaseShift);
  }
};

inline std::optional<AArch64PAuthLoadInstruction>
aarch64PAuthLoadInstruction(uint64_t Word) {
  if (Word > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  Intrinsic Id = Intrinsic::None;
  switch (Word & kPAuthLoadOpcodeMask) {
#define NEVERD_A64_PAUTH_LOAD_OPERATION(Name, Mnemonic, Encoding)              \
  case Encoding:                                                               \
    Id = Intrinsic::Name;                                                      \
    break;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.def"
  default:
    return std::nullopt;
  }
  AArch64PAuthLoadInstruction Instruction{Id, static_cast<uint32_t>(Word)};
  if (Instruction.writeback() && !Instruction.stackBase() &&
      Instruction.destination() == Instruction.base())
    return std::nullopt;
  return Instruction;
}

inline bool aarch64PAuthLoadShapeIsValid(Intrinsic Id, unsigned NumInputs,
                                         unsigned OutputBytes,
                                         unsigned BaseBytes, unsigned WordBytes,
                                         std::optional<uint64_t> Word) {
  if (!Word || NumInputs != kPAuthLoadOperandCount + 1 ||
      OutputBytes != kPAuthLoadPairBytes ||
      BaseBytes != kPAuthLoadPointerBytes || WordBytes != kPAuthLoadWordBytes)
    return false;
  const auto Instruction = aarch64PAuthLoadInstruction(*Word);
  return Instruction && Instruction->Id == Id;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64PAUTHLOAD_H
