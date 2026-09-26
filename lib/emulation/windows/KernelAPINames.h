//===- KernelAPINames.h - Canonical WDM API spellings ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Symbol names shared by modeled call sites come from the API inventory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELAPINAMES_H
#define NEVERD_EMULATION_WINDOWS_KERNELAPINAMES_H

#include "llvm/ADT/StringRef.h"

namespace neverd::emulation::kernel_api {
#define NEVERD_KERNEL_API(Name, Arity, Availability)                           \
  constexpr llvm::StringLiteral Name = #Name;
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
} // namespace neverd::emulation::kernel_api

#endif
