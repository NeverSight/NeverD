#pragma once
#include "CodeEdits.h"
#include "DeferredRelease.h"
#include "PageDelta.h"
#include "Protocol.h"
#include "RenderStore.h"
#include "ResourceLimits.h"

#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/support/ProjectFiles.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd::worker {
class ComparisonService;
class LineageService;
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
  using EncodedPayload = std::shared_ptr<const EncodedJson>;
  struct Reply {
    Json value;
    EncodedPayload encoded;
  };
  Json execute(const std::string &operation, const Json &payload);
  /// The transport uses encoded when present; other operations return value.
  Reply executeResponse(const std::string &operation, const Json &payload);
  using LoadProgressSink =
      std::function<void(const char *Phase, std::uint64_t Done,
                         std::uint64_t Total, const char *Detail)>;
  void setLoadProgressSink(LoadProgressSink Sink) {
    loadProgress_ = std::move(Sink);
  }
  /// The active request owns this thread-safe cancellation probe.
  void setCancellationProbe(std::function<bool()> Probe) {
    cancelled_ = std::move(Probe);
  }
  static bool comparesBinaries();
  std::string revision() const { return std::to_string(revision_); }
  std::string projectId() const { return projectId_; }
  bool analyzed() const { return analyzed_; }
  /// Background work (the reference index) runs between requests.
  bool hasIdleWork() const;
  bool idleStep();
  /// {state, done, total, generation} for heartbeats; null before a load.
  Json backgroundState() const;
  static std::string version();
  /// Whether the engine exposes its static x87 support matrix, independently
  /// of binary loading, host probing or execution.
  static bool reportsX87Capabilities();
  /// Whether versioned machine-source recovery is available for source views.
  static bool recoversInterpreterSource();
  /// Whether the engine can query unwind metadata at an exact address.
  static bool reportsUnwindFrames();
  /// Whether the engine keeps function edits (function_create/_delete).
  static bool keepsFunctionEdits();
  /// Whether the engine keeps what the user states of a function
  /// (function_edit, function_attributes).
  static bool statesFunctionAttributes();
  /// Whether the engine keeps the user's data items.
  static bool keepsDataItems();
  static bool keepsBytePatches();
  static bool keepsSourcePatches();
  static bool provesSourcePatches();
  /// Whether the engine keeps how the user shows operands' numbers.
  static bool keepsOperandFormats();
  /// Whether the engine lists the ways it can load a file (identify).
  static bool identifiesFiles();
  static bool loadsPdbFiles();
  /// Whether the engine lists the program's local types (local_types,
  /// local_type, local_types_source).
  static bool listsLocalTypes();
  /// Whether the engine adds declared types (local_types_declare).
  static bool declaresLocalTypes();
  /// Whether the engine lists the type libraries and takes the user's
  /// choice of them (type_libraries, type_libraries_set).
  static bool choosesTypeLibraries();
  /// Whether the engine ranks local types by the code's accesses through a
  /// pointer (local_types_fit) and names structure offsets by them.
  static bool fitsLocalTypes();
  /// Whether the engine gives a function the user's C declaration, as IDA's
  /// Y does (function_type, and "type" in function_edit).
  static bool typesFunctions();
  /// Whether the engine finds the PDB a PE names before opening it, and
  /// opens a PE without it.
  static bool findsPdbFiles();
  /// Whether the engine finds the DWARF of an ELF or Mach-O image before
  /// opening it, and opens the image without it.
  static bool findsDwarf();
  /// Whether the engine finds an image's static symbol table before opening
  /// it, and opens the image as if stripped.
  static bool findsSymbolTables();
  /// Whether the engine finds the linker MAP beside an input before opening
  /// it, and opens the image without it.
  static bool findsMapFiles();
  /// Whether the engine loads a chosen MAP file into an open image
  /// (map_inspect, map_load).
  static bool loadsMapFiles();
  /// Whether the engine loads a chosen DWARF file into an open image
  /// (dwarf_load).
  static bool loadsDwarfFiles();
  /// Whether the engine finds call paths and call trees (call_paths,
  /// call_tree).
  static bool findsCallPaths();

private:
  Json executeImpl(const std::string &operation, const Json &payload,
                   EncodedPayload *encoded);
  std::unique_ptr<ComparisonService> comparison_;
  std::unique_ptr<LineageService> lineage_;
  std::function<bool()> cancelled_;
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
  static constexpr std::size_t MaxGraphs =
      ::neverd::limits::worker::kEngineMaxGraphs;
  std::unique_ptr<Listing> listing_;
  // The function whose restricted pipeline the session currently holds.
  std::optional<std::uint64_t> preparedFunction_;
  std::uint64_t revision_ = 0;
  std::string projectId_;
  Json openOptions_, signatureInputs_ = Json::array();
  Json pdbInput_ = nullptr, pdbDisplayNames_ = Json::array();
  /// {"mode":"none"} when the user declined the image's DWARF, the file
  /// chosen for it as {"path","stamp"}, else null.
  Json dwarfInput_ = nullptr;
  /// {"mode":"none"} when the user declined the MAP beside the input, the
  /// MAP file chosen for it as {"path","mode","stamp"}, else null.
  Json mapInput_ = nullptr;
  /// {"mode":"none"} when the user declined the image's static symbol
  /// table, else null.
  Json symbolsInput_ = nullptr;
  /// A read-only replica is checked against the owner's loaded input and
  /// committed sidecars before any expensive operation is admitted.
  Json analysisSnapshot();
  /// analysisSnapshot, stating \p state of the user's facts.
  Json analysisSnapshot(Json state);
  Json browseCacheIdentity();
  /// \p identity, a snapshot, with what else says what a cached page read: the
  /// project's files but for \p unread.
  Json browseCacheIdentity(Json identity,
                           const std::vector<project_files::File> &unread);
  Json restoreAnalysis(const Json &snapshot);
  Json updateAnalysisNames(const Json &snapshot);
  /// Whether the project's files hold \p snapshot's user state.
  bool projectHolds(const std::string &path, const Json &snapshot);
  /// Waits, at most the replica limit, for an owner's write \p snapshot was
  /// given before.
  void awaitOwnerWrite(const std::string &path, const Json &snapshot);
  /// A replica takes the function rows that changed as the owner took them,
  /// checked against the table, and false where it must read the project's
  /// file.  Declarations of types it reads from the file: one that names a type
  /// taken away and declared again reads otherwise taken in place.
  bool takeFunctionRows(const Json &before, const Json &after);
  /// A replica takes the names that changed as the owner took them, row by row,
  /// and false where it must read the table again.
  bool takeNames(const std::map<std::uint64_t, std::string> &before,
                 const std::map<std::uint64_t, std::string> &after);
  Json restoredSnapshot_;
  Json restoredDebugInputs_ = Json::array();
  void localNamesChanged();
  Json userState() const;
  Json codeEdits_ = Json::array();
  void reloadCodeEdits();
  static Json readCodeEdits(const std::string &binary);
  Json codeEditRow(std::uint64_t address) const;
  std::string inputHash() const;
  bool analyzed_ = false;
  bool dirty_ = false;
  /// An analysis-section edit no save has written yet. Project files synced
  /// from elsewhere replace the comments, not these edits.
  bool sectionsDirty_ = false;
  bool readOnly_ = false;
  bool browseCacheEnabled_ = true;
  /// Whether idle time analyzes the open file: function discovery and the
  /// reference index.  A cross-reference request still builds the index.
  bool backgroundAnalysis_ = true;
  /// Only clients coordinating analysis-replica memory opt in to SDK snapshots.
  bool steppedDiscovery_ = false;
  /// Whether idle time then prepares each function's analysis
  /// (neverd_warm_functions_json), so that its first decompile reuses it.
  /// Any change to the project starts over: the engine may have dropped it.
  bool warmFunctions_ = false;
  enum class WarmState { Off, Pending, Running, Complete, Full, Unavailable };
  WarmState warmState_ = WarmState::Off;
  std::uint64_t warmCursor_ = 0;
  std::size_t warmPrepared_ = 0;
  /// Whether the engine refused a function for lack of room.  The pass goes
  /// on to the last function: each page still leaves the answers a
  /// decompile reads of calls to its functions.
  bool warmFull_ = false;
  std::string warmError_;
  void restartWarming();
  void warmStep();
  Json warmJson() const;
  /// Whether idle time solves the image's call-register summaries for this
  /// session's decompiles (neverd_prepare_call_summaries_json), after
  /// preparing every function a page at a time for that solve to read.  An
  /// analysis replica opts in at restore. Decompiles from then on read the
  /// image's summaries, so a function's text can change once.
  enum class SummaryState {
    Off,
    Pending,
    Preparing,
    Solving,
    Ready,
    Unavailable,
    Deferred
  };
  SummaryState summaryState_ = SummaryState::Off;
  /// Whether the restore asked for the job, which a change to the project
  /// then runs again; an explicit request's summaries are only forgotten.
  bool summaryJob_ = false;
  std::uint64_t summaryCursor_ = 0;
  /// Whether the job discovered the image's functions, whose summaries it
  /// then solves (summaryJobStep).
  bool summaryDiscovered_ = false;
  std::string summaryError_;
  /// Once the summaries are ready, the job reads ahead the signature bodies
  /// of the callees later decompiles read (neverd_prefill_callees_json),
  /// from prefillCursor_ in the engine's order, until it has read them all,
  /// the engine's memory for them is full, or the engine has none to read.
  enum class PrefillState { Off, Pending, Done, Unavailable };
  PrefillState prefillState_ = PrefillState::Off;
  std::uint64_t prefillCursor_ = 0;
  std::uint64_t prefillPrepared_ = 0;
  bool prefillPending() const;
  void prefillStep();
  /// A replica restored with render_ahead renders each function's engine
  /// pages in idle time, once its summaries and read-ahead are done, smallest
  /// function first (EngineRenderAhead.cpp).  namedView builds a first visit's
  /// document from them as it builds a later visit's from a kept document's.
  /// They have a kept document's lifetime: forgetNamedViews drops them.  What
  /// the job renders also goes to renderStore_, for later sessions over the
  /// same inputs, and a function kept there is not kept here.
  enum class RenderAheadState { Off, Pending, Done, Full };
  bool renderAheadRequested_ = false;
  RenderAheadState renderAheadState_ = RenderAheadState::Off;
  /// Each function's size and entry, smallest first, once read.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> renderAheadOrder_;
  bool renderAheadOrdered_ = false;
  std::size_t renderAheadCursor_ = 0;
  unsigned renderAheadStarts_ = 0;
  std::uint64_t renderAheadRendered_ = 0, renderAheadFailed_ = 0,
                renderAheadHits_ = 0;
  std::size_t renderAheadBytes_ = 0, renderAheadRawBytes_ = 0;
  /// Compact engine pages by namedView's source key.
  std::unordered_map<std::string, std::vector<std::string>> renderedAhead_;
  /// A function rendered under a declaration the user is about to state
  /// (prepare_function with "declaration"), with the declaration it had given
  /// back: its compact pages, what the render read first, as renderedAhead_
  /// keeps them.  A view takes them while what the render read holds, the
  /// store's inputs are those it was made under and the summaries are those
  /// it read; the next declaration made so replaces it.
  struct DeclaredRender {
    std::uint64_t address = 0;
    std::string representation, optionText, inputs;
    std::uint64_t summaries = 0;
    bool summariesHeld = false;
    std::vector<std::string> pages;
  };
  std::optional<DeclaredRender> declaredRender_;
  /// The views a DeclaredRender answered (render_ahead.declared_hits).
  std::uint64_t declaredRenderHits_ = 0;
  /// Moves as the engine takes the image's call summaries or lets them go,
  /// which changes what every render reads (DeclaredRender::summaries).
  std::uint64_t summariesEpoch_ = 0;
  /// What the store's inputs read now, as a digest (renderStoreIdentity).
  std::string renderInputsDigest();
  /// Whether the engine holds the image's call summaries.
  bool callSummariesHeld();
  /// prepare_function with "declaration": renders the function at \p entry as
  /// stating \p declaration would make it read, and gives its declaration
  /// back (DeclaredRender).
  Json prepareUnderDeclaration(std::uint64_t entry,
                               const std::string &declaration);
  std::unique_ptr<RenderStore> renderStore_;
  /// The compact bytes in renderedAhead_, which count against the job's limit
  /// with the store's.
  std::size_t renderAheadHeldBytes_ = 0;
  /// The job started over after it began: this session's engine no longer
  /// reads as a new session's does, whatever the store keeps for those.
  bool renderStoreStale_ = false;
  /// Earlier sessions rendered every function, so the summaries and read-ahead
  /// wait for a request that asks for more than their renders answer.
  bool summariesDeferred_ = false;
  /// The function a view last asked for that no session could render: asking
  /// the engine again reports the same, and does not need the summaries.
  std::optional<std::uint64_t> renderFailedAddress_;
  RenderStore *liveRenderStore() const {
    return renderStoreStale_ ? nullptr : renderStore_.get();
  }
  /// Opens the store a restore asks for by \p mode: "read", "write" or empty.
  void openRenderStore(const std::string &mode);
  /// A request that takes only what is kept (kept_only): what no session kept
  /// is refused as "not_kept", and the engine is not asked.
  bool keptOnly_ = false;
  /// The store a view reads.  A restored one follows its job.  An owner holds
  /// one to read only, and follows what the identity reads, as for the facts.
  RenderStore *storeOfViews();
  std::string rendersStoreName_;
  std::uint64_t rendersStoreGeneration_ = 0;
  bool rendersStoreRead_ = false;
  std::chrono::steady_clock::time_point rendersRefreshed_{};
  /// `kept_renders`: how many renders the store keeps, and whether a job
  /// reached every function.
  Json keptRendersJson();
  /// What the job keeps of what a function's first look in the listing asks
  /// the engine, under the same identity as the renders and for the same
  /// sessions.  The owner reads it too, and takes up what its replica adds.
  std::unique_ptr<RenderStore> factsStore_;
  /// The name of what factsStore_ reads, and the browse cache's generation an
  /// owner last compared it at.
  std::string factsStoreName_;
  std::uint64_t factsStoreGeneration_ = 0;
  bool factsStoreRead_ = false;
  std::chrono::steady_clock::time_point factsRefreshed_{};
  std::uint64_t factsHits_ = 0, factsMisses_ = 0;
  /// The folder names of the stores this session reads, or none.
  struct StoreNames {
    std::string renders, facts;
  };
  StoreNames storeNames();
  /// What the stores are kept under (renderStoreIdentity), or none where this
  /// session keeps no stores.
  std::optional<Json> storeIdentity();
  /// The names of the stores kept under \p identity.
  static StoreNames storeNames(Json identity);
  /// What the render just made read of what the user states, kept as the first
  /// page of its record, and whether what is kept under a record still reads
  /// the same.  Empty, and holding, for an engine that does not tell.
  std::string renderReads();
  bool renderReadsHold(const std::string &compact);
  /// What data is called now, where that is all that changed of what the render
  /// read; null otherwise.
  Json renderReadsRenamed(const std::string &compact);
  /// That of the render the next page of a view replays: a page answers only if
  /// decorating it spells them as a render would.
  Json keptRenamed_;
  bool keptRenderHolds(RenderStore &store, const std::string &key);
  /// Whether an engine says what each render read, so that the stores do not
  /// move for what the user states of one address.
  bool rendersSayTheirReads() const;
  /// What analysis reads of the user's declarations, which no render's reads
  /// say.
  Json userFactsAnalysisReads() const;
  /// The job goes over a whole store for what the user changed.
  bool renderAheadVerifying_ = false;
  /// Says that what the stores keep under \p before holds for the state now,
  /// whose stores are named \p after where the caller knows them.
  void keepStoresFor(const StoreNames &before);
  void keepStoresFor(const StoreNames &before, const StoreNames &after);
  void openFactsStore(const std::string &name, bool writable);
  std::vector<std::string> keptListingFacts(const std::string &key);
  void keepListingFact(const std::string &key, std::string bytes);
  Json listingFactsJson() const;
  void keepListingFacts(std::uint64_t address);
  std::string renderStoreMode_;
  /// What renders depend on: browseCacheIdentity without what a view's
  /// decoration reads, local and function names and comments.
  Json renderStoreIdentity();
  /// A view's key in the store: its source key without the generation, which
  /// counts this session's listing builds.
  std::string renderStoreKey(std::uint64_t address,
                             const std::string &representation,
                             const std::string &optionText);
  /// The rows of the function table without what the user notes of a function,
  /// which nothing analyzed or rendered reads, and, unless \p declarations,
  /// what the user declares, which only the decompiler reads.
  static Json functionRowsRead(const Json &rows, bool declarations);
  /// A function's or a type's declaration changed: the engine has let go of
  /// what read it, and every view reads anew over what the replica still holds.
  /// With \p viewsReadIt the stores stand and the caller has noted the edit for
  /// the views (keepNamedViewsThrough): a document that did not read it stays.
  void declarationsChanged(bool viewsReadIt = false);
  /// Only functions' names changed, in a replica: what it keeps of a view stays
  /// the view's.  Taking a name away from one (\p unnamed) changes what the
  /// listing shows it as, and the others are read as a view is decorated.
  void functionNamesChanged(bool unnamed);
  /// Before an edit that keeps views: what an earlier one left for the next
  /// view to forget goes now, and namedViewsTakeRevision answers for this edit
  /// alone.
  void settleNamedViews();
  /// An edit at \p address, with \p names it took and gave up, reached the
  /// views that read it, and no other.  With \p pagesStale such a document
  /// goes, as its engine pages spell what changed; otherwise it builds again
  /// from them. Each document reads this as it is next asked for
  /// (readViewEdits).
  /// What decoration alone writes of an edit into a copy of a document it
  /// reached: a comment of the image's (CodeEdits::takeComments) or a
  /// function's new name (CodeEdits::takeFunctionNames); and a data's new
  /// name, which its engine pages spell and respelling the engine text
  /// decoration read writes too (CodeEdits::takeDataNames).
  enum class ViewEditKind { Other, Comment, FunctionName, DataName };
  void keepNamedViewsThrough(std::uint64_t address,
                             std::vector<std::string> names, bool pagesStale,
                             ViewEditKind kind = ViewEditKind::Other);
  struct ViewEdit {
    std::uint64_t address;
    std::vector<std::string> names;
    bool pagesStale;
    ViewEditKind kind;
  };
  std::deque<ViewEdit> viewEdits_;
  std::uint64_t viewEditCount_ = 0;
  void readViewEdits(const std::string &sourceKey);
  /// What views keep stands through the revision an edit moved to, and with
  /// \p storesStand so do the stores open: nothing the edit changed is read
  /// into what they are kept under.
  void namedViewsTakeRevision(bool storesStand);
  std::uint64_t cacheGenerationBefore_ = 0;
  void forgetGraphsAndText();
  /// What the render a view replays read, in the form neverd_user_reads wrote.
  std::string keptReads_;
  /// A document reads what the render its pages came from read, one a store
  /// kept or the engine's own, and what its decoration looked up.
  struct NamedView;
  void noteRenderReads(NamedView &document, std::string_view record) const;
  void noteDecorationReads(NamedView &document, const Json &view,
                           std::uint64_t function) const;
  /// Starts at one so that documents never hold a key from before a forget.
  std::uint64_t namedViewsEpoch_ = 1;
  /// open.write_behind: an edit's reply need not wait for its write.
  bool writeBehind_ = false;
  void persistEdit(ProjectHistory &store, Json state);
  /// The listing took a new name of data in place (Listing::dataRenamed).
  void dataNameChanged();
  /// The listing took the user's data items in place
  /// (Listing::dataItemsChanged).
  void dataItemsChanged();
  /// Whether a kept \p page of a view can state the functions it names as they
  /// are named now: it states them as the engine knew them when it rendered,
  /// and a new name replaces those, but what one was called before a name was
  /// taken away is only the engine's to say.
  bool keptPageReadsCurrentNames(const Json &page) const;
  /// The session's renames as neverd_renames_json lists them, read once for all
  /// a revision's views: a project can hold thousands.
  /// The image's names and comments, read once for each revision that does not
  /// take them in place (namedViewsTakeRevision).
  const CodeEdits::ImageFacts &imageFacts() const;
  mutable CodeEdits::ImageFacts imageFacts_;
  mutable std::optional<std::uint64_t> imageFactsRevision_;
  bool imageFactsHeld_ = false;
  /// States the names of \p view's functions as they are named now.
  void currentNamesInView(Json &view) const;
  /// When the last request was answered.
  std::chrono::steady_clock::time_point lastRequest_{};
  /// The function a request last prepared and how long that took.
  std::optional<std::uint64_t> requestPrepared_;
  std::chrono::steady_clock::duration requestPrepareTime_{};
  bool renderingAhead_ = false;
  bool renderAheadPending() const;
  /// False while the step only waits for the reader to pause.
  bool renderAheadStep();
  void renderFunctionAhead(std::uint64_t address);
  /// Whether what the engine renders now reads as it will from here on.
  bool rendersSettled() const;
  /// Keeps the engine pages of a view a request rendered, once they read as a
  /// render of the job does: a function the job leaves to its visit, or has not
  /// reached, costs the next session nothing.
  void keepVisitedRender(std::uint64_t address,
                         const std::string &representation,
                         const std::string &optionText,
                         const std::vector<std::string> &pages);
  void restartRenderAhead();
  Json renderAheadJson() const;
  /// The pages kept of the view \p sourceKey names, or none.
  std::vector<std::string> renderedAheadPages(std::uint64_t address,
                                              const std::string &representation,
                                              const std::string &optionText,
                                              const std::string &sourceKey);
  bool summaryWorkPending() const;
  void restartSummaries();
  /// False where the step only waits on discovery (summaryJobStep).
  bool summaryStep();
  bool summaryJobStep();
  Json summaryJson() const;
  Json prepareCallSummaries();
  /// Drop the decompiled text, views and graphs kept from before the
  /// engine's analysis changed.
  void forgetDecompiledViews();
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
    std::string sourceKey;
    // Retain the exact undecorated document in wire form. A second parsed
    // row tree can exceed the cache budget before the displayed one does.
    // An engine document keeps the pages the engine wrote and the
    // representation they were asked in, from which namedView builds it
    // again as it did; a recovered one keeps its whole text.
    std::string original;
    std::vector<std::string> enginePages;
    std::string engineRepresentation;
    Json value;
    std::vector<std::size_t> lines;
    /// Every address an edit of which reaches this document, sorted, where they
    /// are known: its render's reads and what its decoration looked up.
    std::vector<std::uint64_t> reads;
    bool readsKnown = false;
    /// Unique among the documents of this process: its pages' identity.
    std::uint64_t id = 0;
    /// The line its function's definition begins on, as decoration read it.
    std::optional<std::size_t> definitionLine;
    /// Which edits that reached it decoration writes again: comments and the
    /// functions renamed, which are written into a copy of it, or anything
    /// else.
    mutable bool commentsReached = false, othersReached = false;
    mutable std::vector<std::uint64_t> renamedFunctions, renamedData;
    /// The engine text its decoration read, for an engine document of C on one
    /// page: what a data's new name respells (CodeEdits::takeDataNames).
    std::shared_ptr<const CodeEdits::EngineText> engineText;
    /// What the render its engine pages came from read (neverd_user_reads),
    /// which says whether a data's new name is a spelling alone.
    std::string renderReads;
    /// Its engine pages spell a name of data it has taken since: it never
    /// builds again from them, but from what is kept.
    bool pagesRenamed = false;
    /// Of the edits views have kept through, how many it has read.
    mutable std::uint64_t viewEditsRead = 0;
    /// Hashes of the identifiers its text spells, sorted, once an edit's names
    /// are first looked for in it.
    mutable std::vector<std::uint64_t> spelled;
    mutable bool spelledRead = false;
    // Decide once after decoration. Older engines may return unsorted rows
    // or overlapping name spans; those retain their original order while
    // filtering cached numeric bounds.
    bool orderedRows = false, orderedReferences = false;
    bool orderedSourceNames = false, orderedCodeNames = false;
    std::vector<std::pair<std::size_t, std::size_t>> rowBounds, referenceBounds,
        sourceNameBounds, codeNameBounds;
    std::size_t bytes = 0;
    // Pages share this document's exact identity and invalidation lifetime.
    // Their byte allowance is part of the document cache's existing limit.
    struct EncodedPage {
      std::size_t start, end;
      EncodedPayload value;
      std::size_t bytes;
    };
    mutable std::list<EncodedPage> pages;
    mutable std::size_t pageBytes = 0;
  };
  /// Destroys the documents the cache lets go, after the request that let
  /// them go has answered.
  DeferredRelease releases_;
  /// Newest first, bounded by retained bytes as well as document count.
  std::list<std::shared_ptr<const NamedView>> namedViews_;
  /// Where namedViews_ holds the document of each key, and of each source
  /// key, which no two of them share.  A document's key is read after its
  /// function's preparation, its source key before; preparing a whole
  /// program can change the listing between them.
  using NamedViewPosition =
      std::list<std::shared_ptr<const NamedView>>::iterator;
  std::unordered_map<std::string, NamedViewPosition> namedViewsByKey_;
  std::unordered_map<std::string, NamedViewPosition> namedViewsBySource_;
  /// Takes \p position's document out of both indexes.
  void unindexNamedView(NamedViewPosition position);
  std::size_t namedViewBytes_ = 0, encodedPageBytes_ = 0;
  std::uint64_t namedViewsRevision_ = 0, namedViewsListingGeneration_ = 0;
  /// The listing's whole generation documents were decorated under.
  std::uint64_t namedViewsGeneration_ = 0;
  /// Revisions that changed local display names alone, which drop nothing the
  /// engine prepared (localNamesChanged).
  std::uint64_t localNameRevisions_ = 0;
  /// Forgets the documents of another revision or listing generation.
  void syncNamedViews();
  std::string namedViewSourceKey(std::uint64_t address,
                                 const std::string &representation,
                                 const std::string &optionText);
  /// \p options with what the project's edits add to a source view of
  /// \p address; \p stale receives why it leaves any out.
  Json namedViewEngineOptions(std::uint64_t address,
                              const std::string &representation,
                              const Json &options, const Json *variableEdits,
                              const Json *callTypes, const Json *stackFrame,
                              Json &stale);
  /// The engine's page of \p address from line \p next, its text checked.
  std::string engineViewPage(std::uint64_t address,
                             const std::string &representation,
                             std::size_t next, bool sourceView,
                             const Json &engineOptions,
                             const std::string &optionText);
  void forgetNamedViews();
  void forgetEncodedPages();
  std::shared_ptr<const NamedView>
  namedView(std::uint64_t address, const std::string &representation,
            const Json &options = Json::object(),
            const Json *variableEdits = nullptr,
            const Json *callTypes = nullptr, const Json *stackFrame = nullptr);
  std::optional<Json> namedViewPage(std::uint64_t address,
                                    const std::string &representation,
                                    std::size_t offset, std::size_t limit,
                                    const Json &options = Json::object(),
                                    EncodedPayload *encoded = nullptr,
                                    const std::string *ifNoneMatch = nullptr,
                                    bool pageDelta = false);
  std::uint64_t namedViewIds_ = 0;
  /// Documents written from another with its comments, functions' names or
  /// data's names taken (takeComments, takeFunctionNames, takeDataNames).
  std::uint64_t commentPatches_ = 0, namePatches_ = 0, dataNamePatches_ = 0;
  /// Documents the engine rendered for, rather than built from kept pages.
  std::uint64_t namedViewRenders_ = 0;
  /// Takes the document at \p position out of the documents kept, as one for
  /// the page deltas of what clients keep.
  void dropNamedView(NamedViewPosition position);
  /// What the session's current render read, as neverd_user_reads writes it.
  std::string userReads();
  /// The data a render that read \p reads spells by another name now, as
  /// neverd_user_reads_renamed_json says it: rows of what each is called, or
  /// null where more than a spelling moves.
  Json userReadsRenamed(const std::string &reads);
  /// Something no edit a document notes changed what decoration reads: each
  /// document decorates again from its pages.
  void decorateNamedViewsAgain();
  /// Measures and indexes \p full as \p document's value and keeps it under
  /// \p key and \p sourceKey, as far as the documents' budget allows, where
  /// \p reusable: the listing it read is still the one views read.
  std::shared_ptr<const NamedView>
  keepNamedView(std::shared_ptr<NamedView> document, Json full,
                const std::string &key, const std::string &sourceKey,
                bool reusable);
  std::string instanceTag_;
  /// Documents views had before they were built again, which pages clients
  /// keep may come from (page_delta): the newest last.
  std::deque<std::shared_ptr<const NamedView>> previousViews_;
  std::size_t previousViewBytes_ = 0;
  void keepPreviousView(std::shared_ptr<const NamedView> document);
  /// The page lines [start, end) of \p document make, without what a reply
  /// says of the project or its name, read in place from \p document.
  PageSource namedViewLines(const NamedView &document, std::size_t start,
                            std::size_t end, bool first) const;
  /// The page a client names with \p etag, where this worker still has the
  /// document it came from, read in place from that document.
  std::optional<PageSource> keptClientPage(const std::string &etag) const;
  /// Function-list order (indices into Listing::functionRows) for the last
  /// filter, sort and listing generation.
  std::string functionOrderKey_;
  std::vector<std::size_t> functionOrder_;
  LoadProgressSink loadProgress_;
  void invalidate();
  /// Read every user-edit sidecar into the Session; false when one fails.
  bool reloadUserState(bool bytePatchesChanged = false);
  /// Whether the local types file holds other declarations than the
  /// Session's: git, another tool or a person changed it.
  bool localTypesChanged();
  /// The session's libraries.json.
  std::filesystem::path typeLibrariesPath() const;
  /// libraries.json's choice of type libraries, {"added","removed"}, empty
  /// lists without the file, or none for a file that does not parse.
  std::optional<Json> typeLibrariesOnDisk();
  /// Whether libraries.json says another choice of type libraries than it
  /// held when the engine last read or wrote it.
  bool typeLibrariesChanged();
  /// libraries.json as the engine last read or wrote it, null where it did
  /// not parse.  Comparing the file with it reads no types, as asking the
  /// engine for its choice would.
  Json typeLibraryChoice_ = nullptr;
  /// The Session's type declarations as the history's rows, each at its
  /// place in the order made; none from an engine that keeps none.
  Json typeDeclarationRows() const;
  /// Read the type declarations again where the history's differ from the
  /// Session's: true when they agree or loaded.  Changed declarations let
  /// the analysis go, as declaring does.
  bool reloadTypeDeclarations();
  /// Record which build of the input the project's files describe, after
  /// the worker wrote one of them.
  void recordProject();
  /// neverd_project_json, or null from an older engine.
  Json projectJson() const;
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
  Json bytePatchRows() const;
  /// A committed patch command already proves invalidation is necessary;
  /// ordinary reloads must discover changes from the before/after inventories.
  bool reloadBytePatches(bool knownChanged = false);
  Json patchOperation(const std::string &Operation, const Json &Payload,
                      EncodedPayload *Encoded = nullptr);
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
  /// A known entry's index, or the index of its bounded recovered owner.
  int functionIndex(std::uint64_t address);
  /// A known entry, its recovered owner, or the still-unowned address.
  std::uint64_t functionEntry(std::uint64_t address);
  /// The control flow graph of the function at \p address, in the engine's
  /// cfg JSON shape.
  Json functionGraph(std::uint64_t address);
  /// What a job kept of functionGraph, for a snapshot, which reads no name from
  /// it; or functionGraph.
  Json keptFunctionGraph(std::uint64_t address);
  static std::string graphFactsKey(std::uint64_t address) {
    return "cfg:" + std::to_string(address);
  }
  static bool validFunctionGraph(const Json &graph) {
    return graph.is_object() && graph.contains("nodes") &&
           graph["nodes"].is_array() && graph.contains("edges") &&
           graph["edges"].is_array();
  }
  /// The function list changed: analysis restarts function by function and
  /// the reference index is built again.
  void functionsChanged();
  void requireLoaded() const;
  void requireWriter() const;
  void analyze();
  /// Analyze a VM image first, as the queries of its function list need.
  void analyzeBeforeFunctionQueries();
  /// The function (or -1) and address a name resolves to: a function's own
  /// name, or a name the listing shows.
  std::optional<std::pair<int, std::uint64_t>>
  resolvedName(const std::string &name);
  /// Run the session's restricted single-function pipeline for \p address
  /// unless whole-image analysis already covers it.
  void prepareFunction(std::uint64_t address,
                       std::string_view representation = {});
  Listing &listing();
  ProjectHistory &history();
  Json metadata() const;
  void loadAnalysisSections(neverd_session_t session,
                            const std::filesystem::path &path);
  void saveAnalysisSections();
  void changedAnalysisSections();
  std::string error() const;
  Json backendJson(const char *owned, bool checkError = false) const;
  /// The text of an engine answer \p owned, checked as backendJson checks
  /// it, before it is parsed.
  std::string backendText(const char *owned, bool checkError) const;
  /// \p text, an engine answer, as backendJson gives it.
  Json parsedBackendText(const std::string &text) const;
};
} // namespace neverd::worker
