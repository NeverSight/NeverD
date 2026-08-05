//===- MapParsers.h - Linker map format parsers (internal) --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations for format-specific linker map parsers.
/// Used by LLDMapLoader to dispatch to the correct parser.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_MAPPARSERS_H
#define NEVERD_DEBUG_MAPPARSERS_H

#include "neverd/debug/DebugContext.h"

#include "llvm/ADT/StringRef.h"

#include <map>

namespace neverd {

//===----------------------------------------------------------------------===//
// Format detection helpers (shared by LLDMapLoader and per-format parsers)
//===----------------------------------------------------------------------===//

inline bool isELFMapHeader(llvm::StringRef Line) {
  return Line.trim().starts_with("VMA") && Line.contains("LMA");
}

inline bool isLLDMapHeader(llvm::StringRef Line) {
  llvm::StringRef Trimmed = Line.trim();
  return Trimmed.starts_with("Address") && Trimmed.contains("Align") &&
         Trimmed.contains("Out") && !Trimmed.contains("Publics");
}

inline bool isGNUldMapHeader(llvm::StringRef Line) {
  return Line.trim().starts_with("Linker script and memory map");
}

//===----------------------------------------------------------------------===//
// Per-format parsers
//===----------------------------------------------------------------------===//

/// ELF-style map parser (ld.lld --Map=).
void parseELFMap(llvm::StringRef Content,
                 std::map<va_t, FunctionSym> &Functions);

/// COFF /lldmap parser (lld-link /lldmap:).
void parseCOFFLLDMap(llvm::StringRef Content,
                     std::map<va_t, FunctionSym> &Functions);

/// MachO-style map parser (ld64.lld -map).
void parseMachOMap(llvm::StringRef Content,
                   std::map<va_t, FunctionSym> &Functions);

/// GNU ld map parser (ld --Map=).
void parseGNUldMap(llvm::StringRef Content,
                   std::map<va_t, FunctionSym> &Functions);

} // namespace neverd

#endif // NEVERD_DEBUG_MAPPARSERS_H
