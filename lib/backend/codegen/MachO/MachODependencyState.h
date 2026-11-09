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
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace neverd {
struct MachOChainedImport {
  int64_t Ordinal = 0, Addend = 0;
  unsigned Flags = 0;
  std::string Name;
};
struct MachOChainedSlot {
  uint64_t Offset = 0, Raw = 0;
};
struct MachOChainedSegment {
  uint64_t VMAddr = 0, VMSize = 0, FileOff = 0, FileSize = 0;
  uint32_t CommandOffset = 0, Protections = 0;
  uint16_t PointerFormat = 0, PageSize = 0;
  std::vector<MachOChainedSlot> Slots;
  uint32_t Flags = 0;
};
/// Owned facts from the strict reader, captured only for an installing caller.
struct MachOChainedFixups {
  uint64_t HeaderVA = 0;
  uint32_t CommandOffset = 0;
  bool Threaded = false;
  unsigned LinkeditIndex = 0;
  std::vector<MachOChainedSegment> Segments;
  std::vector<MachOChainedImport> Imports;
  std::vector<std::string> Libraries;
};
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
readMachODependencyState(llvm::ArrayRef<uint8_t> Bytes,
                         MachOChainedFixups *Chained = nullptr);
llvm::Error preserveMachODependencyState(const MachODependencyState &Before,
                                         const MachODependencyState &After);
} // namespace neverd
#endif
