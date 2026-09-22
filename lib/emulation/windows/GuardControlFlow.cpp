//===- GuardControlFlow.cpp - Exact CFG target enforcement ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The helper policy never grants validity to whole executable regions or
/// silently succeeds for targets missing from the validated image metadata.
///
//===----------------------------------------------------------------------===//

#include "GuardControlFlow.h"

#include "DriverImage.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
GuardControlFlow::GuardControlFlow(const DriverImage &Image)
    : Enabled(Image.Guard.Enabled), Targets(Image.Guard.ValidTargets.begin(),
                                            Image.Guard.ValidTargets.end()) {}

llvm::Error GuardControlFlow::registerExportTarget(uint64_t Address) {
  if (!Address ||
      (Address > 0x00007fffffffffffULL && Address < 0xffff800000000000ULL))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CFG: noncanonical export target");
  Targets.insert(Address);
  return llvm::Error::success();
}

llvm::Error GuardControlFlow::validateTarget(uint64_t Address) const {
  if (!Enabled)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CFG: helper invoked for an inactive image");
  if (!Targets.count(Address))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "CFG: indirect call target is not a declared entry point: 0x" +
            llvm::utohexstr(Address));
  return llvm::Error::success();
}
} // namespace neverd::emulation
