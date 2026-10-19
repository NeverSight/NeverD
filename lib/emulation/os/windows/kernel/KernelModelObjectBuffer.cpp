//===- KernelModelObjectBuffer.cpp - Explicit extension model
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
llvm::Error objectError(const llvm::Twine &Text) {
  return llvm::createStringError(Text);
}
} // namespace
llvm::Error KernelModel::objectBuffer(uint64_t Address, uint32_t Size,
                                      bool IsWrite) const {
  if (!Size)
    return llvm::Error::success();
  if (Version || FiniteFileSystem) {
    if (!Address || Size - 1 > UINT64_MAX - Address || Size > KernelMemoryBytes)
      return objectError("invalid or oversized native object buffer");
  } else if (auto E = runtime::checkBufferRange(Address, Size))
    return E;
  if (auto E = validateGuestAccess(Address, Size, IsWrite))
    return E;
  auto Allowed = Memory.canAccess(Address, Size, IsWrite ? Write : Read);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return objectError("buffer has no required guest permissions");
  return llvm::Error::success();
}
} // namespace neverd::emulation
