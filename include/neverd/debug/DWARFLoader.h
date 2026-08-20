//===- DWARFLoader.h - DWARF debug info loader --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares DWARFDebugContext which loads DWARF debug information from
/// ELF and Mach-O binaries via LLVM's DWARFContext.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DWARFLOADER_H
#define NEVERD_DEBUG_DWARFLOADER_H

#include "neverd/debug/DebugContext.h"

#include <filesystem>
#include <memory>

namespace neverd {

class DWARFDebugContext : public DebugContext {
public:
  ~DWARFDebugContext() override;

  static std::unique_ptr<DWARFDebugContext>
  load(const std::filesystem::path &BinaryPath, BinaryFormat Fmt);

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override;
  std::optional<VariableSym>
  resolveStackPointerVariable(va_t FuncAddr, int64_t Offset) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  bool hasInfo() const override;

private:
  DWARFDebugContext() = default;

  struct Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace neverd

#endif // NEVERD_DEBUG_DWARFLOADER_H
