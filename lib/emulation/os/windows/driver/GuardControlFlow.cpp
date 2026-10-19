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

#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
GuardControlFlow::GuardControlFlow(const DriverImage &Image)
    : Enabled(Image.Guard.Enabled), Targets(Image.Guard.ValidTargets.begin(),
                                            Image.Guard.ValidTargets.end()) {}

llvm::Error GuardControlFlow::registerExportTarget(uint64_t Address) {
  if (!Address || (Address > profile::CanonicalUserMax &&
                   Address < profile::CanonicalKernelMin))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CFG: noncanonical export target");
  if (Targets.size() >= guard::MaximumTargets && !Targets.contains(Address))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CFG: aggregate target limit exhausted");
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
  for (const auto &Validate : Validators)
    if (auto E = Validate())
      return E;
  return llvm::Error::success();
}
llvm::Error GuardControlFlow::addImage(const DriverGuardControlFlow &Image,
                                       llvm::ArrayRef<uint64_t> Exports,
                                       std::function<llvm::Error()> Validate) {
  const auto Entries =
      Image.Enabled ? llvm::ArrayRef(Image.ValidTargets) : Exports;
  for (auto Address : Entries) {
    if (auto E = registerExportTarget(Address))
      return E;
  }
  if (Image.Enabled && Validate)
    Validators.push_back(std::move(Validate));
  Enabled |= Image.Enabled;
  return llvm::Error::success();
}
void GuardControlFlow::swap(GuardControlFlow &Other) noexcept {
  std::swap(Enabled, Other.Enabled);
  Targets.swap(Other.Targets);
  Validators.swap(Other.Validators);
}
} // namespace neverd::emulation
