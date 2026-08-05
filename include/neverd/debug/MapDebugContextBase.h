//===- MapDebugContextBase.h - Shared base for MAP loaders -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common base class for MAP-file-based debug contexts (MSVC, LLD).
/// Provides shared resolveFunction / allFunctions / hasInfo
/// implementations backed by a std::map<va_t, FunctionSym>.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_MAPDEBUGCONTEXTBASE_H
#define NEVERD_DEBUG_MAPDEBUGCONTEXTBASE_H

#include "neverd/debug/DebugContext.h"

#include "llvm/ADT/StringRef.h"

#include <map>

namespace neverd {

class MapDebugContextBase : public DebugContext {
public:
  ~MapDebugContextBase() override = default;

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override;
  std::optional<TypeSym> resolveType(uint64_t) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  bool hasInfo() const override;

  static bool isCOFFMapHeader(llvm::StringRef Line);

  static bool parseSegOffset(llvm::StringRef Token, uint16_t &Seg,
                             uint32_t &Offset);

  /// Parse COFF /MAP content into \p Functions.  Shared between
  /// MSVCMapLoader and LLDMapLoader (which auto-detects this format).
  static void parseCOFFMapContent(llvm::StringRef Content,
                                  std::map<va_t, FunctionSym> &Functions,
                                  uint64_t ImageBase = 0);

  static void parseCOFFMapLineNumbers(llvm::StringRef Content,
                                      std::map<va_t, SourceLoc> &Locations,
                                      uint64_t ImageBase = 0);

protected:
  std::map<va_t, FunctionSym> Functions;
  std::map<va_t, SourceLoc> SourceLocations;
  bool Loaded = false;

  /// Infer function sizes from gaps between adjacent symbol addresses.
  /// Call after populating Functions.
  void inferFunctionSizes();
};

} // namespace neverd

#endif // NEVERD_DEBUG_MAPDEBUGCONTEXTBASE_H
