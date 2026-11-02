#pragma once

#include "OperandFormat.h"
#include "OperandFormatDetail.h"
#include "ProjectHistory.h"
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPITypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd::worker {

/// Address class of the navigation overview and listing prefixes
/// (AddressClasses.def).
enum class AddressClass : std::uint8_t {
#define NEVERD_ADDRESS_CLASS(Name, Code) Name = Code,
#include "AddressClasses.def"
};

/// Address-ordered listing of one loaded image, formatted as an interactive
/// disassembler shows it: segment directives, function headers, labels,
/// instructions, data items and reference comments.  The listing reads only
/// public C ABI queries.  Instruction semantics (transfers, references) come
/// from the engine's lift; this class only names and lays them out.
///
/// The background reference index decodes every known function once, in
/// bounded steps, so a host can interleave it with interactive requests.
class Listing {
public:
  explicit Listing(neverd_session_t session);
  ~Listing();
  Listing(const Listing &) = delete;
  Listing &operator=(const Listing &) = delete;

  /// Names, comments or function boundaries changed.
  void invalidate();
  void configureCache(std::filesystem::path folder,
                      std::function<Json()> identity, bool writable,
                      AtomicWriter writer);
  bool cachePending() const;
  void flushCacheOne();
  void flushCache();
  Json cacheStats() const;
  void setStackFrames(const Json &rows, const std::string &imageHash);
  /// The neverd_strings_ex_json options strings are found with, as JSON
  /// text; empty for the engine's defaults.
  void setStringOptions(std::string options);
  /// Unverified PDB names affect presentation only. No symbol, function
  /// extent, type or instruction fact is installed in the engine.
  void setPdbDisplayNames(const Json &names);
  /// An imported display-only data name that the listing currently shows.
  std::optional<std::string> pdbDataName(std::uint64_t address);
  /// `listing`: lines around an address.
  Json page(const Json &payload);
  /// `overview`: address classes in equal linear buckets.
  Json overview(const Json &payload);
  /// `xrefs` from the reference index or, for "from", the instruction's lift.
  Json references(std::uint64_t address, const Json &payload);
  /// `string_references`: every instruction that refers to a string, directly
  /// or through a pointer slot, as a filtered and sorted table page.
  Json stringReferences(const Json &payload);
  /// `strings`: the strings the listing shows, the user's among them, as a
  /// page of rows {"address","length","type","text","encoding"} in address
  /// order, filtered by text, address or type.
  Json strings(const Json &payload);
  /// The name the listing shows for \p address, automatic or not.
  std::string nameAt(std::uint64_t address);
  /// Resolve a name the listing shows, a function's, a data item's or an
  /// automatic one, to its address.
  std::optional<std::uint64_t> resolveName(const std::string &name);
  /// The entry owning an address, including recovered noncontiguous code.
  /// An unowned address searches a bounded set of nearby function CFGs.
  std::optional<std::uint64_t> chunkOwner(std::uint64_t address);
  /// Whether \p address is an import's slot or the entry of its thunk.
  bool isImport(std::uint64_t address);
  /// Build the reference index again, as for a changed function list.
  void reindex();
  /// Read the user's operand formats again.  They change only instruction
  /// text, so the rest of the listing stands.
  void reloadNumberFormats();
  /// Names or the user's data items changed: the next query reads them again
  /// without scanning strings or reading imports and exports again.
  void namesChanged();
  /// Comments changed. Rendering reads them live, but they are part of the
  /// browse cache identity, so cached documents start a new namespace.
  void commentsChanged();
  /// Whether instruction lines show the user's operand formats.
  bool showsNumberFormats();
  /// How the listing writes the operands a structure offset changes:
  /// "arm" (`[x0, #8]`), "sbf" (`[r1 + 8]`), "evm" (PUSH data), or empty for
  /// x86 listings' spelling.
  std::string_view operandSyntax();
  /// The access the SBF memory operand \p operand of the instruction at
  /// \p address makes by itself; none for another operand or dialect.
  std::optional<operand_format_detail::SbfAccess>
  ownAccess(std::uint64_t address, std::size_t operand);
  /// Which operands of the instruction that starts at \p address a number
  /// format changes, by index; none when no instruction starts there.
  std::optional<std::vector<bool>> numberOperands(std::uint64_t address);
  /// Which of them a structure offset changes: numbers and displacements
  /// from a base register.
  std::optional<std::vector<bool>> structOperands(std::uint64_t address);
  /// Whether the engine names structure offsets by local types.
  bool namesStructOffsets();
  /// The operand of the instruction at \p address a structure offset
  /// changes: \p asked when it can, else its first displacement from a
  /// register, else its last number; none when it has neither or no
  /// instruction starts there.  \p number says whether it is a number,
  /// with its value.
  std::optional<std::size_t>
  structOperand(std::uint64_t address, std::optional<std::size_t> asked,
                std::optional<std::int64_t> *number = nullptr);
  /// The item at \p address as the listing shows it: {"start","size","kind"}
  /// and "user", the kind of the user's data item there; null outside the
  /// image.
  Json item(std::uint64_t address);
  /// The options strings are searched with (JSON), empty for the defaults.
  const std::string &stringOptions() const;
  /// Formatted instruction lines (no prefixes) of [start, end), for graph
  /// nodes.  Each element is {address, text, spans}.
  Json blockLines(std::uint64_t start, std::uint64_t end);
  /// Explicit frame-name displacements used by the listing at this PC.
  /// Ambiguous names are omitted, so assembly cannot guess their offset.
  Json assemblySymbols(std::uint64_t address);
  /// Functions in address order under their display names:
  /// [{"name","engine_name","address","size","library","thunk","exported"}].
  const Json &functionRows();
  /// Engine function name -> display name, for engine text such as
  /// pseudocode that spells functions the engine's way.
  const std::unordered_map<std::string, std::string> &functionAliases();
  /// `names`, `imports`, `exports`, `segments_ex` table rows.
  Json names();
  Json regions();

  /// Read the jump tables of whole-program analysis, once it has run: their
  /// names, slots, comments and references join the listing.
  void loadSwitches();

  /// Opt in only when the client coordinates the snapshot's memory lifetime.
  void setSteppedDiscovery(bool enabled);
  bool hasIdleWork() const;
  /// Advance discovery/reference indexing. False while an owned discovery
  /// snapshot is computing, so the executor can wait for interactive reads.
  /// Completion is a separate step from rebuilding the derived listing.
  bool idleStep();
  /// {state, done, total, generation} of the reference index.
  Json indexState() const;
  /// Changes whenever published listing text can change.
  std::uint64_t generation() const;

private:
  Json uncachedPage(const Json &payload);
  Json uncachedOverview(const Json &payload);
  const Json &uncachedFunctionRows();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace neverd::worker
