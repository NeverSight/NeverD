//===- AArch64DwarfTestsDetail.h - RA CFI fixture builder -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact CIE/FDE bytes shared by static-state and native-boundary tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_LANGUAGE_AARCH64DWARFTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_LANGUAGE_AARCH64DWARFTESTSDETAIL_H

#include "LanguageEHTestsDetail.h"

#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::aarch64_dwarf_test {

#define NEVERD_A64_DWARF_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define NEVERD_A64_DWARF_TEXT(Name, Text) inline constexpr char Name[] = Text;
#define NEVERD_A64_DWARF_PROGRAM(Name, ...)                                    \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "AArch64DwarfFixtures.def"

inline std::vector<uint8_t>
frame(llvm::ArrayRef<uint8_t> Program, llvm::ArrayRef<uint8_t> Initial = {},
      const char *Augmentation = kOrdinary, uint64_t CodeAlign = kCodeAlign,
      uint64_t Text = kText, bool ExtraAugmentationByte = false,
      std::optional<uint64_t> PCRelativeBase = {}) {
  language_eh_test::ByteBuilder B;
  B.u32(0);
  B.u32(0);
  B.u8(kVersion);
  B.str(Augmentation);
  B.uleb(CodeAlign);
  B.sleb(kDataAlign);
  B.u8(kReturnRegister);
  B.uleb(1 + ExtraAugmentationByte);
  B.u8(PCRelativeBase ? kPCRelativeEncoding : kAbsoluteEncoding);
  if (ExtraAugmentationByte)
    B.u8(kSpareAugmentationByte);
  for (uint8_t Byte : Initial)
    B.u8(Byte);
  B.patch32(0, B.size() - sizeof(uint32_t));
  const size_t FDE = B.size();
  B.u32(0);
  B.u32(FDE + sizeof(uint32_t));
  B.u64(PCRelativeBase ? Text - *PCRelativeBase - B.size() : Text);
  B.u64(kRange);
  B.uleb(0);
  for (uint8_t Byte : Program)
    B.u8(Byte);
  B.patch32(FDE, B.size() - FDE - sizeof(uint32_t));
  B.u32(0);
  return B.data();
}

inline dwarf_eh::ParseResult decode(llvm::ArrayRef<uint8_t> Bytes,
                                    Arch Architecture = Arch::AArch64) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  return dwarf_eh::parseEHFrame(Image, {Bytes.data(), Bytes.size(), kFrame},
                                {});
}

inline dwarf_eh::AArch64RAQueryResult
query(const dwarf_eh::ParseResult &Parsed, uint64_t Offset,
      const dwarf_eh::AArch64RAQueryLimits &Limits = {}) {
  return dwarf_eh::evaluateAArch64ReturnAddressState(
      Arch::AArch64, Parsed.CIEs.front(), Parsed.FDEs.front(),
      Parsed.FDEs.front().InitialLocation + Offset, Limits);
}

inline std::vector<uint8_t> absoluteState(uint64_t State, int64_t Offset) {
  language_eh_test::ByteBuilder B;
  B.u8(kSetState);
  B.uleb(State);
  B.sleb(Offset);
  return B.data();
}

} // namespace neverd::aarch64_dwarf_test

#endif // NEVERD_UNITTESTS_LIFT_LANGUAGE_AARCH64DWARFTESTSDETAIL_H
