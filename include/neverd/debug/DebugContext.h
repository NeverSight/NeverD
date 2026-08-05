//===- DebugContext.h - Debug information context ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the DebugContext abstract interface for querying debug
/// information (DWARF, PDB) — function symbols, variable locations,
/// types, and source locations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DEBUGCONTEXT_H
#define NEVERD_DEBUG_DEBUGCONTEXT_H

#include "neverd/Common.h"
#include "neverd/ir/high/HighIR.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct SourceLoc {
  std::string File;
  uint32_t Line = 0;
  uint32_t Col = 0;
};

struct FunctionSym {
  std::string Name;
  va_t Addr = 0;
  uint64_t Size = 0;
  SourceLoc DeclLoc;
  TypeRef ReturnType;
  std::vector<std::pair<std::string, TypeRef>> Params;

  bool contains(va_t Address) const {
    return Address >= Addr && Address - Addr < Size;
  }
};

struct VariableSym {
  std::string Name;
  TypeRef Type;
  int64_t StackOffset = 0;
  bool IsParam = false;
};

struct TypeSym {
  std::string Name;
  TypeRef Type;
};

class DebugContext {
public:
  virtual ~DebugContext() = default;

  virtual std::optional<FunctionSym> resolveFunction(va_t Addr) const = 0;
  virtual std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                                     int64_t Offset) const = 0;
  virtual std::optional<TypeSym> resolveType(uint64_t TypeId) const = 0;
  virtual std::optional<SourceLoc> sourceLocation(va_t Addr) const = 0;

  virtual std::vector<FunctionSym> allFunctions() const = 0;

  virtual bool hasInfo() const = 0;
};

class NullDebugContext : public DebugContext {
public:
  std::optional<FunctionSym> resolveFunction(va_t) const override {
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {}; }
  bool hasInfo() const override { return false; }
};

} // namespace neverd

#endif // NEVERD_DEBUG_DEBUGCONTEXT_H
