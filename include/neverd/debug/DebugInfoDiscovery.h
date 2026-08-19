//===- DebugInfoDiscovery.h - Locate and load debug info ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds the debug information that belongs to a binary and turns it into a
/// DebugContext.  NeverD supports several debug formats through separate
/// loaders (DWARF, PDB, linker MAP); this is the single place that decides
/// which of them applies to a given image and in what order to try them, so
/// every entry point that opens a binary resolves symbols the same way.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DEBUGINFODISCOVERY_H
#define NEVERD_DEBUG_DEBUGINFODISCOVERY_H

#include "neverd/debug/DebugContext.h"

#include <filesystem>
#include <memory>
#include <string>

namespace neverd {

struct BinaryImage;

/// Which loader produced a DebugContext.
enum class DebugInfoKind { None, DWARF, PDB, Map };

/// Human-readable name of \p Kind, for diagnostics and the C API.
const char *debugInfoKindName(DebugInfoKind Kind);

/// Where to look for debug information beyond the binary itself.
struct DebugInfoRequest {
  /// Caller-supplied companion files.  A non-empty path is authoritative:
  /// discovery loads exactly that file and reports an error if it yields
  /// nothing, rather than silently falling back to whatever sits next to the
  /// binary.  Someone who names a file wants to know when it is the wrong one.
  std::filesystem::path PDBPath;
  std::filesystem::path MapPath;

  /// Set false to read the image alone.  The escape hatch for a stale or
  /// mismatched companion file that names functions worse than the image does.
  bool Enabled = true;
};

/// Outcome of a discovery attempt.
struct DebugInfoResult {
  std::unique_ptr<DebugContext> Context;
  DebugInfoKind Kind = DebugInfoKind::None;

  /// File \c Context was built from; empty when nothing was found.
  std::filesystem::path Path;

  /// Set only when the caller named a file that failed to load.  An absent
  /// companion file is a normal outcome and leaves this empty.
  std::string Error;

  /// True once a context carrying at least one symbol was loaded.
  explicit operator bool() const { return Context && Context->hasInfo(); }
};

/// Locate and load the debug information for \p BinaryPath.
///
/// Native debug info comes first because it is both richer and harder to
/// mismatch than a MAP file: PDB for PE, DWARF (including an adjacent .dSYM or
/// a split .debug companion) for ELF and Mach-O.  A linker MAP is the last
/// resort — it carries names and addresses but no types, source lines, or
/// build identity — and exists for stripped binaries whose build only kept the
/// map.
DebugInfoResult loadDebugInfo(const std::filesystem::path &BinaryPath,
                              const BinaryImage &Img,
                              const DebugInfoRequest &Req = {});

/// Publish \p Dbg's function symbols into \p Img's symbol table and return how
/// many names it contributed.
///
/// Debug info is a NameOrigin::Stated source, which puts it level with the
/// image's own symbol table, so an address the image already names keeps that
/// name and only placeholders are replaced.  An address the image does not
/// describe at all gains a function symbol.  Publishing into BinaryImage —
/// rather than keeping the names inside the DebugContext — is what lets every
/// consumer (function list, detector, disassembler, emitters) see them without
/// each having to consult the debug context separately.
unsigned applyDebugSymbols(BinaryImage &Img, const DebugContext &Dbg);

} // namespace neverd

#endif // NEVERD_DEBUG_DEBUGINFODISCOVERY_H
