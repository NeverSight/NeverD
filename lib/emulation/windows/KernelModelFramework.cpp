//===- KernelModelFramework.cpp - WDF module dispatch ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bridge authoritative PE export identities to the independent WDF model.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

namespace neverd::emulation {
namespace {
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME
} // namespace
std::optional<unsigned>
KernelModel::argumentCount(const KernelExportRegistry::Export &Export) {
  if (Export.Kind == KernelExportRegistry::ExportKind::ModuleExport &&
      Export.Module == KernelProvider)
    return argumentCount(Export.Name);
  return KernelFramework::argumentCount(Export);
}

llvm::Expected<uint64_t> KernelModel::call(
    const KernelExportRegistry::Export &Export,
    llvm::ArrayRef<uint64_t> Arguments,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument) {
  if (Export.Kind == KernelExportRegistry::ExportKind::ModuleExport &&
      Export.Module == KernelProvider)
    return call(Export.Name, Arguments, ReadArgument);
  if (!Framework)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "framework model is not initialized");
  return Framework->call(Export, Arguments, CurrentIRQL);
}

std::optional<KernelFramework::GuestCall> KernelModel::takeGuestCall() {
  return Framework ? Framework->takeGuestCall() : std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishGuestCall(uint64_t Token, uint64_t Result) {
  if (!Framework)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "framework callback has no owning model");
  return Framework->finishGuestCall(Token, Result);
}
} // namespace neverd::emulation
