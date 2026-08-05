//===- PDBLoader.h - PDB debug info loader ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares PDBDebugContext which loads PDB debug information from
/// Windows PE binaries via LLVM's PDB library.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_PDBLOADER_H
#define NEVERD_DEBUG_PDBLOADER_H

#include "neverd/debug/DebugContext.h"

#include <filesystem>
#include <memory>

namespace neverd {

class PDBDebugContext : public DebugContext {
public:
  ~PDBDebugContext() override;

  static std::unique_ptr<PDBDebugContext>
  load(const std::filesystem::path &PdbPath, uint64_t ImageBase = 0);

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  bool hasInfo() const override;

private:
  PDBDebugContext() = default;

  struct Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace neverd

#endif // NEVERD_DEBUG_PDBLOADER_H
