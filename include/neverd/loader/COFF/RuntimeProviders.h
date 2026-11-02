//===- RuntimeProviders.h - Reviewed PE runtime providers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Provider-name queries shared by COFF loading, lifting and pipeline proofs.
/// These queries establish no import-storage or invocation authority.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_RUNTIMEPROVIDERS_H
#define NEVERD_LOADER_COFF_RUNTIMEPROVIDERS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

namespace neverd::coff_loader {

#define NEVERD_COFF_RUNTIME_PROVIDERS_BEGIN(Query)                             \
  inline bool Query(llvm::StringRef Module) {                                  \
    return llvm::StringSwitch<bool>(Module)
#define NEVERD_COFF_RUNTIME_PROVIDER(Module) .CaseLower(Module, true)
#define NEVERD_COFF_RUNTIME_PROVIDERS_END()                                    \
  .Default(false);                                                             \
  }
#include "neverd/loader/COFF/RuntimeProviders.def"

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_RUNTIMEPROVIDERS_H
