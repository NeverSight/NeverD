//===- GuestMemory.cpp - Byte-exact guest scalar access -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Byte-exact guest scalar access.
///
//===----------------------------------------------------------------------===//

#include "GuestMemory.h"

#include <array>
namespace neverd::emulation {
llvm::Expected<uint64_t> GuestMemory::readInteger(uint64_t Address,
                                                  unsigned Size) {
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid guest scalar width");
  std::array<uint8_t, 8> Bytes{};
  if (auto E =
          read(Address, llvm::MutableArrayRef<uint8_t>(Bytes.data(), Size)))
    return std::move(E);
  uint64_t Value = 0;
  for (unsigned I = 0; I != Size; ++I)
    Value |= uint64_t(Bytes[I]) << (I * 8);
  return Value;
}
llvm::Error GuestMemory::writeInteger(uint64_t Address, uint64_t Value,
                                      unsigned Size) {
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid guest scalar width");
  std::array<uint8_t, 8> Bytes{};
  for (unsigned I = 0; I != Size; ++I)
    Bytes[I] = static_cast<uint8_t>(Value >> (I * 8));
  return write(Address, llvm::ArrayRef<uint8_t>(Bytes.data(), Size));
}
} // namespace neverd::emulation
