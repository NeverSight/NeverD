#pragma once
#include "Protocol.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
  using Resolver =
      std::function<std::optional<std::uint64_t>(const std::string &)>;
  /// The listing's display names for functions the engine leaves generic,
  /// such as main or start for a sub_ name, by engine name.
  using FunctionAliases = std::unordered_map<std::string, std::string>;
  /// What decoration reads of the image's names and comments, by address: a
  /// project may hold thousands, and a view asks for a few.
  struct ImageFacts {
    /// The address of a name the engine has for something the user renamed.
    std::unordered_map<std::string, std::uint64_t> originals;
    std::unordered_map<std::uint64_t, std::string> names, comments;
    /// From the engine's rows of each.
    void read(const Json &renames, const Json &annotations);
    /// One row of the engine's has changed.
    void renamed(std::uint64_t address, const std::string &original,
                 const std::string &name);
    void commented(std::uint64_t address, const std::string &text);
  };
  /// Decorate a complete source view, preserving its lines and remapping every
  /// UTF-8 span. Edit targets come from declared locals or resolved image
  /// names. An image name shows its rename, otherwise its display name.
  /// \p engine, where given, is what engineText read of the view as it is
  /// passed, and takes the globals its text declares.
  /// Spells \p renamed, rows of what data is called now, in an engine page of C
  /// rendered under the names before them, as a render under these reads.
  /// False, with the page as it was, unless its text shows a spelling is all
  /// that moves.
  static bool respell(Json &page, const Json &renamed);
  /// What decoration reads of an engine view of C that respelling reads again:
  /// its text, where its body begins and the spans its source names cover, as
  /// the engine wrote them, before decorate moves them.
  struct EngineText {
    /// A global as the text declares it, by a marker or an extern's trailing
    /// address.
    struct Global {
      std::uint64_t address;
      std::string name;
      /// By a marker that says only the address, on a line where nothing
      /// before the name could hold another: all a name reaches of the
      /// declaration.
      bool plain;
    };
    std::string text;
    std::size_t bodyStart = 0;
    std::vector<std::pair<std::size_t, std::size_t>> sourceNames;
    /// The globals the text declares, where decorate read them of it: what
    /// respelling asks first, read once.  None where no one read them.
    std::optional<std::vector<Global>> globals;
  };
  /// Of an engine view about to be decorated.
  static EngineText engineText(const Json &view);
  /// A view decorate wrote from \p engine for \p representation, under the code
  /// edit row \p row, after the data in \p renamed, rows of what data is called
  /// now, took those names: the view as decorate writes it from the engine
  /// text respell gives (each name of that data shows its new name and the
  /// spans after it move, a name the text spelled takes the new spelling, its
  /// locals the identity of the new text, the rows of the lines it changed
  /// their anchors, its variable the new name), and that engine text.  Nothing,
  /// with the view as it was, where respell would refuse, or what decorate
  /// reads besides would read otherwise: a number's base or a note anchored on
  /// a changed line, local names that would go stale or come back, or a
  /// variable or identifier symbol the new name may mean; the view is then read
  /// again from what is kept.
  static std::optional<EngineText>
  takeDataNames(Json &view, const EngineText &engine, const Json &renamed,
                const Json &row, const std::string &representation);
  /// \p definitionLine, where given, takes the line the function's definition
  /// begins on, which a function's own comment is written at.
  static void decorate(Json &view, const Json &row, std::uint64_t function,
                       const std::string &representation,
                       const ImageFacts &image,
                       const FunctionAliases &displayNames,
                       const Resolver &resolve,
                       std::optional<std::size_t> *definitionLine = nullptr,
                       EngineText *engine = nullptr);
  /// A view decorate wrote for \p function, its definition on
  /// \p definitionLine, after the image's comments changed and nothing else it
  /// reads: each row's comment is read again as decorate reads it, and where
  /// it changed its line's inline comment is replaced and the spans after it
  /// move, as decorate would write them.  False, with the view as it was,
  /// where a line with a changed comment holds more than one row or does not
  /// end in the comment decorate wrote; the view is then decorated again.
  static bool takeComments(Json &view, std::uint64_t function,
                           std::optional<std::size_t> definitionLine,
                           const ImageFacts &image);
  /// A view decorate wrote, after the functions at \p renamed took the names
  /// \p image gives them: each name the view links to one of them shows its
  /// new name, and the spans after it move, as decorate would write them.
  /// False, with the view as it was, where one of them gave its name up or the
  /// view spells a new name otherwise, which may lead there now; the view is
  /// then decorated again.
  static bool takeFunctionNames(Json &view,
                                const std::vector<std::uint64_t> &renamed,
                                const ImageFacts &image);
  /// Give a complete source view a source name wherever its code spells an
  /// identifier its identifier_symbols list (`fini`, which C calls the
  /// function `_fini`), so the identifier finds the function and reads as its
  /// symbol. Comments and literals spell none.
  static void spellIdentifierSymbols(Json &view);
};
} // namespace neverd::worker
