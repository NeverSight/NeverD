#ifndef NEVERD_LOADER_MACHO_DARWINSOURCEDECLARATIONS_H
#define NEVERD_LOADER_MACHO_DARWINSOURCEDECLARATIONS_H

#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

namespace neverd {
/// Exact SDK data export, including the Mach-O linker spelling and provider.
/// This does not authenticate an import slot or grant object-layout authority.
bool darwinDeclaredSourceDataExport(Arch Architecture, llvm::StringRef Symbol,
                                    llvm::StringRef Module);
std::optional<SourceCallTypeHint>
darwinDeclaredSourceCallHint(const BinaryImage &Image, va_t ImportSlot);
std::optional<SourceCallTypeHint>
darwinDeclaredSourceGlobalAddressHint(const BinaryImage &Image,
                                      va_t ImportSlot);
} // namespace neverd
#endif
