//===- CSourceRecorder.h - Private emission event recorder ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CSOURCERECORDER_H
#define NEVERD_BACKEND_C_CSOURCERECORDER_H

#include "neverd/backend/c/CSourceMap.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace neverd {
struct HighStmt;

/// Records emission events, never a search for printed expressions. Private
/// delimiter renders must reproduce ordinary source byte for byte. HighC can
/// instead record navigation directly on unchanged output, relocating buffered
/// statement spans when their text is appended. Library coverage always uses
/// the independent delimiter proof.
class CSourceRecorder {
public:
  enum class Recording : uint8_t {
    LibraryRegions,
    Instructions,
    /// With no library evidence, one private render can prove both kinds of
    /// navigation location. It must not publish library coverage.
    DefinitionsAndInstructions,
    /// Navigation-only byte positions in the ordinary HighC output. No text
    /// delimiters or library coverage; buffered bodies need explicit
    /// relocation.
    Positions
  };

  CSourceRecorder(CSourceMap &Map, llvm::StringRef Ordinary,
                  Recording Mode = Recording::LibraryRegions);
  void prepareHighSources();
  void prepareLLVMSources(const LLVMSourceMap &Sources);
  std::string expression(va_t Function, const HighExpr &Expr, std::string Text);
  std::string expression(const llvm::Instruction &Value, std::string Text);
  std::optional<size_t> instruction(const llvm::Instruction &Value);
  std::optional<size_t> statement(va_t Function, const HighStmt &Stmt);
  std::optional<size_t> function(va_t Entry);
  std::optional<size_t> function(const llvm::Function &Function);
  std::string begin(size_t Event) const;
  std::string end(size_t Event) const;
  /// \p Text without this recorder's delimiters, held in \p Storage when it
  /// has any: what the ordinary render prints. An emitter that chooses a
  /// spelling from operand text must read this, or the delimiters change the
  /// source it reproduces.
  llvm::StringRef plain(llvm::StringRef Text, std::string &Storage) const;
  /// A delimiter where a top-level function definition begins.
  std::string definition(std::optional<va_t> Entry);
  std::string definition(const llvm::Function &Function);
  bool finish(llvm::StringRef Annotated, llvm::StringRef Ordinary);
  bool recordsPositions() const { return Mode == Recording::Positions; }
  void positionDefinition(std::optional<va_t> Entry, uint64_t Offset);
  void positionStatement(size_t Event, uint64_t Begin, uint64_t End);
  /// Mark the start of a body buffer's anchors, including nested bodies.
  size_t positionBuffer() const { return PositionAnchors.size(); }
  /// Move a completed body's anchors to where its unchanged bytes are appended.
  void appendPositionBuffer(size_t First, uint64_t Size, uint64_t Offset);
  /// Publish only complete, in-bounds positions in this emission's exact text.
  bool finishPositions(llvm::StringRef Ordinary);

private:
  size_t event(std::optional<size_t> Region,
               std::vector<sigs::LibraryOccurrence> Coverage,
               std::optional<va_t> Function = std::nullopt);
  CSourceMap &Map;
  Recording Mode;
  bool recordsInstructions() const { return Mode != Recording::LibraryRegions; }
  std::string Prefix;
  struct Event {
    std::optional<size_t> Region;
    std::vector<sigs::LibraryOccurrence> Coverage;
    std::optional<va_t> Function;
  };
  std::vector<Event> Events;
  std::vector<std::optional<va_t>> DefinitionEntries;
  std::vector<CSourceDefinition> PositionDefinitions;
  std::vector<CSourceAnchor> PositionAnchors;
  bool PositionsValid = true;
  std::map<std::pair<va_t, const HighExpr *>, Event> HighRegions;
  std::map<std::pair<va_t, const HighExpr *>, std::set<sigs::LibraryOccurrence>>
      HighCallAnchors;
  std::map<std::pair<va_t, va_t>, Event> HighStores;
  std::optional<HighStatementSourceIndex> HighStatements;
  std::map<const llvm::Function *, va_t> LLVMFunctions;
  std::map<const llvm::Value *, Event> LLVMRegions;
  std::map<const llvm::Instruction *,
           std::pair<va_t, std::set<sigs::LibraryOccurrence>>>
      LLVMAnchors;
};

} // namespace neverd

#endif
