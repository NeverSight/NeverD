//===- COFFRegistrationEHEncoding.cpp - PE32 chain stores ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode publication boundaries independently of a compiler's register choice.
//===----------------------------------------------------------------------===//
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::coff_loader {
std::optional<uint8_t> getX86RegistrationChainStoreSize(const BinaryImage &Img,
                                                        va_t Address) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || Address > UINT32_MAX - 5)
    return std::nullopt;
  const auto *Bytes = Img.readVA(Address, 6);
  if (!Bytes || Bytes[0] != 0x64)
    return std::nullopt;
  uint8_t Size = 0;
  if (Bytes[1] == 0xa3 && readLE<uint32_t>(Bytes + 2) == 0)
    Size = 6;
  else if (Address <= UINT32_MAX - 6 && (Bytes = Img.readVA(Address, 7)) &&
           Bytes[1] == 0x89 && (Bytes[2] & 0xc7) == 0x05 &&
           readLE<uint32_t>(Bytes + 3) == 0)
    Size = 7;
  if (!Size || !Img.hasExecutableCodeOwnerRange(Address, Size))
    return std::nullopt;
  return Size;
}
} // namespace neverd::coff_loader
