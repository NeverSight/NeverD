//===- KernelModelExports.cpp - Windows routine lookup -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolves counted guest Unicode routine names without host pointer access.
///
//===----------------------------------------------------------------------===//

#include "KernelExportRegistry.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
llvm::Expected<uint64_t> KernelModel::resolveRoutine(uint64_t Address) {
  if (!Exports)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "kernel export registry is unavailable");
  if (Address > UINT64_MAX - windows::UnicodeRecordSize)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "routine name record overflows");
  if (auto E = validateGuestAccess(Address, windows::UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + windows::UnicodeMaximumOffset, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + windows::UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || *Length > *Maximum ||
      *Length > profile::MaxKernelExportNameSize * 2 ||
      *Buffer > UINT64_MAX - *Length)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid kernel routine UNICODE_STRING");
  if (auto E = validateGuestAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (!Bytes.empty())
    if (auto E = Memory.read(*Buffer, Bytes))
      return E;
  std::string Name;
  for (size_t I = 0; I < Bytes.size(); I += 2) {
    if (Bytes[I + 1] || Bytes[I] < 0x21 || Bytes[I] > 0x7e)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unsupported kernel export name encoding");
    Name.push_back(static_cast<char>(Bytes[I]));
  }
  return Exports->resolve(Name);
}
} // namespace neverd::emulation
