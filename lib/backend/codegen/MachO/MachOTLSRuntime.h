//===- MachOTLSRuntime.h - Darwin per-thread initialization -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Build the Darwin TLV calling-convention resolver for a published
/// image.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_TLSRUNTIME_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_TLSRUNTIME_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {
struct MachOTLSResolver {
  std::vector<uint8_t> Code;
  uint64_t ConfigDeltaOffset;
};
/// Config contains the anchor descriptor, guard offset, template address,
/// field and callback counts, {offset, schema} pairs, then IA/0 callbacks.
llvm::Expected<MachOTLSResolver> buildMachOTLSResolver();
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_TLSRUNTIME_H
