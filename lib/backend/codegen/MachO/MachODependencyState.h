//===- MachODependencyState.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate and preserve native dyld bindings.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_DEPENDENCYSTATE_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_DEPENDENCYSTATE_H
#include "neverd/object/MachOLayout.h"

#include "llvm/Support/Error.h"

#include <array>
#include <map>
#include <vector>
namespace neverd {
struct MachODependencyState {
  uint32_t CPUType = 0, CPUSubtype = 0;
  std::array<uint8_t, 32> Digest;
  std::map<std::pair<uint64_t, unsigned>, std::vector<uint8_t>> Slots;
  std::vector<std::vector<uint8_t>> Libraries, Symbols, Indirect;
  std::vector<uint8_t> Exports;
};
// Unlike the analysis loader this decoder rejects every malformed or unmodeled
// byte. Its result is evidence for editing loader metadata, not best effort
// data.
llvm::Expected<MachODependencyState>
readMachODependencyState(llvm::ArrayRef<uint8_t> Bytes);
llvm::Error preserveMachODependencyState(const MachODependencyState &Before,
                                         const MachODependencyState &After);
} // namespace neverd
#endif
