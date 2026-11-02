//===- LinuxKernelAvailability.h - Explicit absent interfaces ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXKERNELAVAILABILITY_H
#define NEVERD_EMULATION_LINUXKERNELAVAILABILITY_H
#include "LinuxKernel.h"
namespace neverd::emulation::linux_model {
enum class IOVectorImportKind { CopyAll, SingleBuffer };
std::optional<uint32_t> gkiPidFDFlags(AndroidGKIKernel Kernel);
std::optional<uint32_t> gkiPidFDNonLeaderError(AndroidGKIKernel Kernel);
std::optional<IOVectorImportKind> gkiIOVectorImport(AndroidGKIKernel Kernel);
std::optional<LinuxXAttrQueryImport>
xattrQueryImport(const LinuxKernelOptions &Options);
llvm::Error validateKernelOptions(const LinuxKernelOptions &Options);
llvm::Error validateKernelTaskInputs(const ProcessOptions &Options);
bool unavailableKernelService(ServiceKind Kind,
                              const std::optional<LinuxKernelOptions> &Options);
} // namespace neverd::emulation::linux_model
#endif
