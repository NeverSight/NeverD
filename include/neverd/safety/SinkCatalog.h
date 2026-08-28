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

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neverd::safety {

/// One catalog entry describing a dangerous call target and the argument slots
/// the engine must reason about.  Argument indices are zero-based positions in
/// the recovered call-argument list; -1 marks a slot the routine does not have.
struct SinkEntry {
  std::string Name; ///< canonical normalized name.
  std::vector<std::string> Aliases;
  VulnClass Class = VulnClass::Unknown;
  SinkKind Kind = SinkKind::Copy;

  int DstArg = -1;    ///< destination buffer.
  int SrcArg = -1;    ///< source buffer (its length bounds an implicit copy).
  int LenArg = -1;    ///< explicit copy length / formatted-write limit.
  int CapArg = -1;    ///< explicit destination capacity (fortified variants).
  int FmtArg = -1;    ///< format-string argument.
  int HandleArg = -1; ///< the freed / reallocated handle (audit).
  bool UnboundedWrite = false; ///< the call itself accepts unbounded input.
  /// The release API reports failure and therefore cannot be treated as a
  /// must-free event unless its result is proved successful.
  bool ReleaseMayFail = false;

  unsigned Severity = 50;

  /// The single argument whose value decides the checked property: the format
  /// string for format sinks, otherwise an explicit length or implicit source.
  int decidingArg() const {
    if (Kind == SinkKind::Format && FmtArg >= 0)
      return FmtArg;
    if (LenArg >= 0)
      return LenArg;
    if (SrcArg >= 0)
      return SrcArg;
    if (FmtArg >= 0)
      return FmtArg;
    return -1;
  }
};

/// Discovery metadata for one external-input provider.  These fields describe
/// the intended source roles but do not by themselves grant executable taint
/// or memory effects; consumers must also resolve a matching closed-world call
/// effect.
struct SourceEntry {
  std::string Name;
  /// The argument index that receives tainted bytes, or -1 when there is no
  /// output-buffer argument.
  int OutArg = -1;
  /// Declared return-value role.  An absent value preserves the catalog's
  /// legacy convention that a source without an output argument describes its
  /// return.  A matching call effect is still required before analysis may
  /// execute that role.
  std::optional<bool> TaintedReturn;

  bool returnCarriesInput() const { return TaintedReturn.value_or(OutArg < 0); }
};

/// Executable semantics supplied by a configured sink.  The semantic family
/// is deliberately closed: catalog JSON may make a copy or printf-style
/// formatting contract executable, but cannot assemble arbitrary effects.
struct ConfiguredCallEffect {
  static constexpr unsigned VariadicArity =
      std::numeric_limits<unsigned>::max();

  enum class Family : uint8_t { None, Copy, Format };
  enum class Format : uint8_t {
    Unconstrained = 0,
    ELF = uint8_t{1} << 0,
    COFF = uint8_t{1} << 1,
    MachO = uint8_t{1} << 2,
  };
  enum class ABI : uint8_t {
    Unconstrained = 0,
    SysV = uint8_t{1} << 0,
    Microsoft = uint8_t{1} << 1,
    Darwin = uint8_t{1} << 2,
    AAPCS = uint8_t{1} << 3,
  };

  Family TheFamily = Family::None;
  Format Formats = Format::Unconstrained;
  ABI ABIs = ABI::Unconstrained;
  unsigned MinArity = 0;
  unsigned MaxArity = 0;
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
  /// Return a configured executable-effect override.  A returned entry whose
  /// family is None explicitly shadows any built-in effect for that spelling.
  const ConfiguredCallEffect *
  matchConfiguredCallEffect(llvm::StringRef StatedName) const;

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
  llvm::StringMap<unsigned> SinkIndex; ///< normalized name/alias -> SinkList.
  std::vector<SourceEntry> SourceList;
  llvm::StringMap<unsigned> SourceIndex; ///< normalized name -> SourceList.
  std::vector<ConfiguredCallEffect> ConfiguredEffectList;
  llvm::StringMap<unsigned> ConfiguredEffectIndex;
  llvm::StringMap<bool> ConfiguredSourceEffectShadows;
  ConfiguredCallEffect ConfiguredNoEffect;

  void setConfiguredSinkEffect(const SinkEntry &Entry,
                               ConfiguredCallEffect Effect);
};

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SINKCATALOG_H
