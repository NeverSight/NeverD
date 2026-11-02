#pragma once
#include "Protocol.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace neverd::worker {
/// Durable source edits. Names/comments are presentation; variable types are
/// checked engine assertions passed into the source emitter, never text
/// substitutions or authority for machine analysis and native rewriting.
class CodeEdits {
public:
  static void validateRow(const Json &row);
  static Json change(Json row, const Json &request);
  /// Refuse obsolete or ambiguous targets before a durable edit is committed.
  static void validateChange(const Json &view, const Json &request);
  /// State affecting analysis, excluding only local display names and their
  /// presentation identities. Unknown fields remain part of the comparison.
  static Json analysisState(const Json &rows);
  using Resolver =
      std::function<std::optional<std::uint64_t>(const std::string &)>;
  /// The listing's display names for functions the engine leaves generic,
  /// such as main or start for a sub_ name, by engine name.
  using FunctionAliases = std::unordered_map<std::string, std::string>;
  /// Decorate a complete source view, preserving its lines and remapping every
  /// UTF-8 span. Edit targets come from declared locals or resolved image
  /// names. An image name shows its rename, otherwise its display name.
  static void decorate(Json &view, const Json &row, std::uint64_t function,
                       const std::string &representation, const Json &renames,
                       const Json &annotations,
                       const FunctionAliases &displayNames,
                       const Resolver &resolve);
  /// Give a complete source view a source name wherever its code spells an
  /// identifier its identifier_symbols list (`fini`, which C calls the
  /// function `_fini`), so the identifier finds the function and reads as its
  /// symbol. Comments and literals spell none.
  static void spellIdentifierSymbols(Json &view);
};
} // namespace neverd::worker
