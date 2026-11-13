//===- MachOExportLayout.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Relocate checked Mach-O export tries.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_EXPORTLAYOUT_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_EXPORTLAYOUT_H
#include "neverd/object/MachOLayout.h"

#include "llvm/Support/Error.h"

#include <map>
namespace neverd {
struct MachOChainedFixups;
struct MachOExportLayout {
  uint64_t Flags = 0, Address = 0, Resolver = 0, Ordinal = 0;
  std::string ImportName;
  bool operator==(const MachOExportLayout &) const = default;
};
using MachOExportMap = std::map<std::string, MachOExportLayout>;
llvm::Expected<MachOExportMap>
readMachOExportLayout(llvm::ArrayRef<uint8_t> Bytes);
llvm::Expected<std::vector<uint8_t>>
writeMachOExportLayout(const MachOExportMap &Exports);
// Canonical absolute-address identities; the distinguished linker header
// follows the real header rather than pretending to be ordinary code/data.
llvm::Expected<std::vector<uint8_t>>
machOExportIdentities(llvm::ArrayRef<uint8_t> Bytes, uint64_t HeaderVA,
                      uint32_t FileType, MachOChainedFixups *Chained);
} // namespace neverd
#endif
