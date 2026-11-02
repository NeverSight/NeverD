//===- X86CxxRuntimeABI.h - Known PE32 C++ runtime providers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_X86CXXRUNTIMEABI_H
#define NEVERD_LOADER_COFF_X86CXXRUNTIMEABI_H

#include "neverd/loader/COFF/RuntimeProviders.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::coff_loader {

/// Providers of the PE32 FH3 cdecl dispatcher and the two-word stdcall throw
/// entry. Each caller must still check the architecture, exact import name,
/// slot, veneer and language records. A module name alone proves none of them.
inline bool hasKnownX86CxxExceptionRuntimeABI(llvm::StringRef Module) {
  return isX86CxxExceptionProvider(Module);
}

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_X86CXXRUNTIMEABI_H
