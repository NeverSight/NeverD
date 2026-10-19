//===- KernelModelDeclaredVersion.cpp - Explicit extension model
//----------------===//
// NeverD Decompiler
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit extension model.
///

#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Text) {
  return llvm::createStringError(Text);
}
llvm::Error objectError(const llvm::Twine &Text) {
  return llvm::createStringError(Text);
}
} // namespace
llvm::Expected<uint64_t> KernelModel::getVersion(uint64_t Address) {
  if (!Version)
    return modelError("RtlGetVersion requires an explicit windows_version");
  if (auto E = runtime::checkBufferRange(Address, 4))
    return E;
  if (auto E = validateGuestAccess(Address, 4, false))
    return E;
  auto Size = Memory.readInteger(Address, 4);
  if (!Size)
    return Size.takeError();
  // Observed ntoskrnl 10.0.19041: always write the four basic version words,
  // even for size zero. The size and service-pack string remain untouched.
  if (*Size == 292)
    return modelError("RTL_OSVERSIONINFOEX2 is outside the version profile");
  const bool Extended = *Size == 284;
  if (auto E = runtime::checkBufferRange(Address, Extended ? 284 : 20))
    return E;
  if (auto E = validateGuestAccess(Address + 4, 16, true))
    return E;
  if (Extended)
    if (auto E = validateGuestAccess(Address + 276, 8, true))
      return E;
  for (const auto &[Offset, Length] :
       {std::pair<uint64_t, uint64_t>{4, 16},
        std::pair<uint64_t, uint64_t>{276, Extended ? 8 : 0}}) {
    if (!Length)
      continue;
    auto Allowed = Memory.canAccess(Address + Offset, Length, Write);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return modelError("RtlGetVersion output is not writable");
  }
  std::array<uint8_t, 16> Bytes{};
  llvm::support::endian::write32le(Bytes.data(), Version->Major);
  llvm::support::endian::write32le(Bytes.data() + 4, Version->Minor);
  llvm::support::endian::write32le(Bytes.data() + 8, Version->Build);
  llvm::support::endian::write32le(Bytes.data() + 12,
                                   2); // VER_PLATFORM_WIN32_NT
  if (auto E = Memory.write(Address + 4, Bytes))
    return E;
  if (Extended) {
    const uint8_t Extra[] = {0, 0, 0, 0, 0, 0, 1, 0}; // NT workstation
    if (auto E = Memory.write(Address + 276, Extra))
      return E;
  }
  return uint64_t(StatusSuccess);
}
} // namespace neverd::emulation
