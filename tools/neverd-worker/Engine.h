#pragma once
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPISession.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::worker {
class ProjectLock;
class ProjectHistory;
class Contributions;
class GraphSnapshot;
class Listing;
class Engine {
public:
  Engine();
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Json execute(const std::string &operation, const Json &payload);
  using LoadProgressSink =
      std::function<void(const char *Phase, std::uint64_t Done,
                         std::uint64_t Total, const char *Detail)>;
  void setLoadProgressSink(LoadProgressSink Sink) {
    loadProgress_ = std::move(Sink);
  }
  std::string revision() const { return std::to_string(revision_); }
  std::string projectId() const { return projectId_; }
  bool analyzed() const { return analyzed_; }
  /// Background work (the reference index) runs between requests.
  bool hasIdleWork() const;
  void idleStep();
  /// {state, done, total, generation} for heartbeats; null before a load.
  Json backgroundState() const;
  static std::string version();
  /// Whether the engine keeps function edits (function_create/_delete).
  static bool keepsFunctionEdits();
  /// Whether the engine keeps the user's data items.
  static bool keepsDataItems();
  /// Whether the engine keeps how the user shows operands' numbers.
  static bool keepsOperandFormats();
  /// Whether the engine lists the ways it can load a file (identify).
  static bool identifiesFiles();

private:
  neverd_session_t session_ = nullptr;
  std::unique_ptr<ProjectLock> lock_;
  std::unique_ptr<ProjectHistory> history_;
  std::unique_ptr<Contributions> contributions_;
  /// A function's graph laid out for the client metrics it was sized with.
  struct LaidOutGraph {
    std::string address, metrics, revision;
    std::unique_ptr<GraphSnapshot> snapshot;
  };
  /// The graphs shown last, the newest first: returning to one neither
  /// analyzes its function again nor lays it out.
  std::list<LaidOutGraph> graphs_;
  static constexpr std::size_t MaxGraphs = 64;
  std::unique_ptr<Listing> listing_;
  // The function whose restricted pipeline the session currently holds.
  std::optional<std::uint64_t> preparedFunction_;
  std::uint64_t revision_ = 0;
  std::string projectId_;
  Json openOptions_, signatureInputs_ = Json::array();
  /// A read-only replica is checked against the owner's loaded input and
  /// committed sidecars before any expensive operation is admitted.
  Json analysisSnapshot();
  Json restoreAnalysis(const Json &snapshot);
  Json userState() const;
  Json codeEdits_ = Json::array();
  void reloadCodeEdits();
  static Json readCodeEdits(const std::string &binary);
  Json codeEditRow(std::uint64_t address) const;
  std::string inputHash() const;
  bool analyzed_ = false;
  bool dirty_ = false;
  bool readOnly_ = false;
  /// Whether idle time analyzes the open file: function discovery and the
  /// reference index.  A cross-reference request still builds the index.
  bool backgroundAnalysis_ = true;
  std::uintmax_t loadedSize_ = 0;
  std::filesystem::file_time_type loadedTime_;
  /// neverd_strings_ex_json options as JSON text, empty for the defaults; a
  /// workbench preference that outlives the open file.
  std::string stringOptions_;
  Listing &newListing();
  std::string textKey_, textCache_;
  std::vector<std::size_t> textLines_;
  /// A completed, self-contained view. It borrows no Session or IR state, so
  /// another function may replace the restricted pipeline while this survives.
  struct NamedView {
    std::string key;
    Json value;
    std::vector<std::size_t> lines;
    std::size_t bytes = 0;
  };
  /// Newest first, bounded by retained bytes as well as document count.
  std::list<std::shared_ptr<const NamedView>> namedViews_;
  std::size_t namedViewBytes_ = 0;
  std::uint64_t namedViewsRevision_ = 0, namedViewsListingGeneration_ = 0;
  void forgetNamedViews();
  std::shared_ptr<const NamedView> namedView(std::uint64_t address,
                                             const std::string &representation);
  std::optional<Json> namedViewPage(std::uint64_t address,
                                    const std::string &representation,
                                    std::size_t offset, std::size_t limit);
  /// Function-list order (indices into Listing::functionRows) for the last
  /// filter, sort and listing generation.
  std::string functionOrderKey_;
  std::vector<std::size_t> functionOrder_;
  LoadProgressSink loadProgress_;
  void invalidate();
  /// Read every user-edit sidecar into the Session; false when one fails.
  bool reloadUserState();
  /// Read the function edits sidecar: true when it loaded or the engine keeps
  /// no function edits.  A changed function list restarts its analysis.
  bool reloadFunctionEdits();
  /// The engine's function edits as rows; none from an older engine.
  Json functionEditRows() const;
  /// Read the data items sidecar: true when it loaded or the engine keeps no
  /// data items.  Changed items show in the listing.
  bool reloadDataItems();
  /// The user's data items as rows; none from an older engine.
  Json dataItemRows() const;
  /// Read the operand formats sidecar: true when it loaded or the engine
  /// keeps no operand formats.  Changed formats show in the listing.
  bool reloadOperandFormats();
  /// The user's operand formats as rows; none from an older engine.
  Json operandFormatRows() const;
  /// The user's operand formats changed: instruction text shows them at
  /// once, without building the listing again.
  void operandFormatsChanged();
  /// Names or the user's data items changed: drop what shows them, and let
  /// the listing read them again without its string scan.
  void namesChanged();
  /// A comment changed.  The listing reads comments as it formats lines, so
  /// only views that keep formatted text drop it.
  void commentsChanged();
  /// The control flow graph of the function at \p address, in the engine's
  /// cfg JSON shape.
  Json functionGraph(std::uint64_t address);
  /// The function list changed: analysis restarts function by function and
  /// the reference index is built again.
  void functionsChanged();
  void requireLoaded() const;
  void requireWriter() const;
  void analyze();
  /// Run the session's restricted single-function pipeline for \p address
  /// unless whole-image analysis already covers it.
  void prepareFunction(std::uint64_t address);
  Listing &listing();
  ProjectHistory &history();
  Json metadata() const;
  std::string error() const;
  Json backendJson(const char *owned, bool checkError = false) const;
};
} // namespace neverd::worker
