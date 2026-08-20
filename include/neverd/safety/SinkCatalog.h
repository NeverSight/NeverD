//===- SinkCatalog.h - Dangerous-call and input-source catalog --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A configurable table of dangerous call targets ("sinks") and external input
/// providers ("sources").  The default table is the X-macro lists in
/// SafetySinks.def and SafetySources.def; lookup normalizes a stated callee
/// name (leading-underscore stripping, alias folding) so one entry matches
/// every per-format spelling.  A specification file can extend or replace
/// entries.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SINKCATALOG_H
#define NEVERD_SAFETY_SINKCATALOG_H

#include "neverd/safety/SafetyTypes.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>
#include <vector>

namespace neverd::safety {

/// One catalog entry describing a dangerous call target and the argument slots
/// the engine must reason about.  Argument indices are zero-based positions in
/// the recovered call-argument list; -1 marks a slot the routine does not have.
struct SinkEntry {
  std::string Name;               ///< canonical normalized name.
  std::vector<std::string> Aliases;
  VulnClass Class = VulnClass::Unknown;
  SinkKind Kind = SinkKind::Copy;

  int DstArg = -1; ///< destination buffer.
  int SrcArg = -1; ///< source buffer (its length bounds an implicit copy).
  int LenArg = -1; ///< explicit copy length / element count.
  int CapArg = -1; ///< explicit destination capacity (fortified variants).
  int FmtArg = -1; ///< format-string argument.
  int HandleArg = -1; ///< the freed / reallocated handle (audit).

  unsigned Severity = 50;

  /// The single argument whose value decides the checked property: an explicit
  /// length when present, otherwise the source whose length is implicit.
  int decidingArg() const {
    if (LenArg >= 0)
      return LenArg;
    if (SrcArg >= 0)
      return SrcArg;
    if (FmtArg >= 0)
      return FmtArg;
    return -1;
  }
};

/// One external-input provider whose return value or output buffer is treated
/// as attacker-controlled by the argument prefilter.
struct SourceEntry {
  std::string Name;
  /// The argument index that receives the tainted bytes, or -1 when the taint
  /// arrives through the return value (getenv, recv-less read wrappers, ...).
  int OutArg = -1;
};

/// A folded view of the catalog with normalized lookup.
class SinkCatalog {
public:
  /// The built-in table from SafetySinks.def / SafetySources.def.
  static SinkCatalog defaults();

  /// Merge entries from a specification file, overriding by canonical name.
  /// The format is a small self-describing object; unknown fields are ignored.
  llvm::Error mergeSinksFromFile(llvm::StringRef Path);
  llvm::Error mergeSourcesFromFile(llvm::StringRef Path);

  /// Look a stated callee name up after normalization.  Returns nullptr when
  /// the name is not a known sink.
  const SinkEntry *matchSink(llvm::StringRef StatedName) const;
  const SourceEntry *matchSource(llvm::StringRef StatedName) const;

  void addSink(SinkEntry E);
  void addSource(SourceEntry E);
  /// Register an extra spelling for an already-added canonical sink.
  void addSinkAlias(llvm::StringRef Canonical, llvm::StringRef Alias);

  /// Reduce a stated per-format spelling to the canonical catalog key: strip
  /// the leading underscores a platform prepends to C symbols.  Fortified and
  /// mangled spellings are matched through registered aliases rather than by
  /// rewriting them here, so their distinct argument layout is preserved.
  static std::string normalize(llvm::StringRef StatedName);

  size_t sinkCount() const { return SinkList.size(); }
  size_t sourceCount() const { return SourceList.size(); }

private:
  std::vector<SinkEntry> SinkList;
  llvm::StringMap<unsigned> SinkIndex;   ///< normalized name/alias -> SinkList.
  std::vector<SourceEntry> SourceList;
  llvm::StringMap<unsigned> SourceIndex; ///< normalized name -> SourceList.
};

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SINKCATALOG_H
