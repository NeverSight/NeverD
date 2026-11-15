//===- MIPSCodeMode.h - Occurrence and callable mode evidence ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate occurrence spans and serialized callable tags against local modes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_MIPSCODEMODE_H
#define NEVERD_LOADER_ELF_MIPSCODEMODE_H

#include "neverd/loader/BinaryImageModel.h"

namespace neverd::mips {

/// Check every local mode crossed by a unit, including zero-sized symbols.
inline bool codeModeRangeMatches(const BinaryImage &Image, va_t Start,
                                 uint64_t Size, InstructionMode Mode) {
  if (!hasCompressedMIPSCode(Image.Mode))
    return true;
  if (!Size || Size > InvalidVA - Start ||
      Image.instructionModeAt(Start, Mode) != Mode ||
      Image.instructionModeAt(Start + Size - 1, Mode) != Mode)
    return false;
  if (!Image.MIPS)
    return true;
  const auto &Profile = *Image.MIPS;
  const va_t End = Start + Size;
  for (auto It = Profile.CodeModeEntries.lower_bound(Start);
       It != Profile.CodeModeEntries.end() && It->first < End; ++It)
    if (It->second != Mode)
      return false;
  auto Region = std::upper_bound(
      Profile.CodeRegions.begin(), Profile.CodeRegions.end(), Start,
      [](va_t Address, const MIPSCodeRegion &R) { return Address < R.Start; });
  if (Region != Profile.CodeRegions.begin())
    --Region;
  for (; Region != Profile.CodeRegions.end() && Region->Start < End; ++Region)
    if (Region->End > Start && Region->Mode != Mode)
      return false;
  return true;
}

/// Machine callable pointer -> instruction VA. Direct CALL/JALX operands and
/// code-byte addresses are different roles and must not use this conversion.
inline std::optional<va_t> serializedCodeTarget(const BinaryImage &Image,
                                                va_t Raw) {
  if (!hasCompressedMIPSCode(Image.Mode))
    return normalizeCodeAddress(Raw, Image.Arch, Image.Mode);
  if (!Image.is64Bit() && Raw > UINT32_MAX)
    return std::nullopt;
  const va_t Address = Raw & ~va_t{1};
  const auto Mode = Image.instructionModeAt(Address);
  if (!Mode || Address % instructionModeAlignment(*Mode) ||
      serializeCodePointer(Address, Image.Arch, *Mode) != Raw)
    return std::nullopt;
  return Address;
}

} // namespace neverd::mips

#endif // NEVERD_LOADER_ELF_MIPSCODEMODE_H
