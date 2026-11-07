//===- EngineRenderAhead.cpp - First visits from idle-time renders --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A replica restored with render_ahead renders its functions' engine pages
/// in idle time, smallest function first, and keeps them compact.  A first
/// visit then builds its document from those pages, as a visit after a local
/// rename builds one from a kept document's, without preparing the function.
///
/// The pages are what the engine answers for the same source key, and they
/// have a kept document's lifetime: whatever forgets the documents forgets
/// them and starts the job over.  What the replica decompiles depends on what
/// it knows, so the job waits for the summaries and the read-ahead.  From there
/// a view reads the same whichever functions came first, but for the code
/// addresses a prepared function publishes to the image (CodeRefTargets): like
/// a kept document, a render reads what the engine knew when it ran, so a
/// function reads as it does after the functions rendered before it.
///
/// What the job renders is kept for later sessions over the same inputs
/// (RenderStore.h), which read it from their first request on, and render only
/// what no session has reached.  A session whose job has reached every function
/// needs neither the summaries nor the read-ahead for those renders, and solves
/// them only when a request asks for more.
//===----------------------------------------------------------------------===//

#include "BrowseCache.h"
#include "Engine.h"
#include "Listing.h"
#include "PageCodec.h"
#include "RenderStore.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/support/ProjectFiles.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neverd::worker {
namespace {
/// How long no request must have come before rendering a function of \p size.
std::chrono::milliseconds holdOff(std::uint64_t size) {
  namespace limits = ::neverd::limits::worker;
  return std::chrono::milliseconds(
      limits::kRenderAheadHoldOffMilliseconds +
      static_cast<std::int64_t>(size) *
          limits::kRenderAheadHoldOffMillisecondsPerKilobyte / 1024);
}

/// Kept once a job has reached every function.
const std::string CompleteKey = "complete";
} // namespace

bool Engine::keptRenderHolds(RenderStore &store, const std::string &key) {
  const auto first = store.readFirst(key);
  return !first || renderReadsHold(*first);
}

Json Engine::renderStoreIdentity() {
  auto identity = browseCacheIdentity();
  auto &state = identity.at("state");
  state.erase("code_edits");
  state.erase("annotations");
  state.erase("operands");
  // Only the listing lays out what the user defines of data.
  state.erase("items");
  // Each render says which names of data and declarations it read, and answers
  // while they read the same (renderReadsHold).  A function's name is not
  // spelled in its pages at all.
  if (rendersSayTheirReads()) {
    state["functions"] = functionRowsRead(state.at("functions"), false);
    state.erase("renames");
    identity["user_facts_analysis_reads"] = userFactsAnalysisReads();
  } else {
    state["functions"] = functionRowsRead(state.at("functions"), true);
    Json dataNames = Json::array();
    for (auto row : state.value("renames", Json::array()))
      if (neverd_func_find_by_addr(
              session_, parseAddress(row.at("addr").get<std::string>())) < 0) {
        row.erase("original");
        dataNames.push_back(std::move(row));
      }
    state["renames"] = std::move(dataNames);
  }
  for (const auto file :
       {project_files::File::Code, project_files::File::Annotations,
        project_files::File::Renames, project_files::File::Operands,
        project_files::File::Functions, project_files::File::Items})
    identity.at("shared").erase(
        std::string(project_files::describe(file).Path));
  return identity;
}

std::string Engine::renderStoreKey(std::uint64_t address,
                                   const std::string &representation,
                                   const std::string &optionText) {
  return hexAddress(address) + ":" + representation + ":" + optionText;
}

namespace {
/// What a page says of a function's name, for each function it says it of.
template <typename Page, typename Each>
void eachStatedFunction(Page &page, Each each) {
  const auto address = [](const auto &json, const char *field) {
    const auto at = json.find(field);
    return at != json.end() && at->is_string()
               ? std::optional(parseAddress(at->template get<std::string>()))
               : std::nullopt;
  };
  if (const auto identity = page.find("function_identity");
      identity != page.end() && identity->is_object())
    if (const auto entry = address(*identity, "addr"))
      each(*entry, *identity);
  if (const auto regions = page.find("library_regions");
      regions != page.end() && regions->is_array())
    for (auto &region : *regions)
      if (const auto identity = region.find("callee_identity");
          identity != region.end() && identity->is_object())
        if (const auto entry = address(*identity, "addr"))
          each(*entry, *identity);
}
} // namespace

const Json &Engine::renameRows() const {
  if (renameRowsRevision_ != revision_) {
    renameRows_ = backendJson(neverd_renames_json(session_));
    renameRowsRevision_ = revision_;
  }
  return renameRows_;
}

bool Engine::keptPageReadsCurrentNames(const Json &page) const {
  bool current = true;
  const auto &names = renameRows();
  eachStatedFunction(page, [&](std::uint64_t address, const Json &identity) {
    if (identity.value("name_origin", std::string()) != "user")
      return;
    current =
        current &&
        std::any_of(names.begin(), names.end(), [&](const auto &row) {
          return parseAddress(row.at("addr").template get<std::string>()) ==
                 address;
        });
  });
  return current;
}

void Engine::currentNamesInView(Json &view) const {
  const auto &rows = renameRows();
  if (rows.empty())
    return;
  std::unordered_map<std::uint64_t, std::string> names;
  for (const auto &row : rows)
    names.emplace(parseAddress(row.at("addr").get<std::string>()),
                  row.at("renamed").get<std::string>());
  eachStatedFunction(view, [&](std::uint64_t address, Json &identity) {
    if (const auto name = names.find(address); name != names.end()) {
      identity["name"] = identity["display_name"] = name->second;
      identity["name_origin"] = identity["display_origin"] = "user";
    }
  });
}

Engine::StoreNames Engine::storeNames() {
  const auto project = projectJson();
  if (!browseCacheEnabled_ || !project.is_object() ||
      !project.contains("folder"))
    return {};
  auto identity = renderStoreIdentity();
  const auto &runtime = BrowseCache::runtimeIdentity();
  StoreNames names;
  names.renders = BrowseCache::digest(runtime + identity.dump());
  // What a look asks the engine spells no name of data either.
  identity.at("state").erase("renames");
  names.facts = BrowseCache::digest(runtime + identity.dump());
  return names;
}

namespace {
std::filesystem::path projectLocalFolder(const Json &project,
                                         const char *folder) {
  const auto &root = project.at("folder").get_ref<const std::string &>();
  return std::filesystem::path(std::u8string(
             reinterpret_cast<const char8_t *>(root.data()), root.size())) /
         "local" / folder;
}
} // namespace

void Engine::openRenderStore(const std::string &mode) {
  renderStore_.reset();
  renderStoreStale_ = false;
  summariesDeferred_ = false;
  if (mode.empty())
    return;
  if (mode != "read" && mode != "write")
    throw Error("invalid_request", "render_store is read or write");
  try {
    const auto names = storeNames();
    if (names.renders.empty())
      return;
    renderStore_ = std::make_unique<RenderStore>(
        projectLocalFolder(projectJson(), "render-ahead"), names.renders,
        mode == "write");
    if (names.facts != factsStoreName_)
      openFactsStore(names.facts, mode == "write");
  } catch (const std::exception &) {
    // The job keeps what it renders in memory, and later sessions render it
    // again.
    return;
  }
  summariesDeferred_ = summaryJob_ && renderStore_->contains(CompleteKey);
}

void Engine::keepStoresFor(const StoreNames &before) {
  rendersStoreRead_ = false;
  factsStoreRead_ = false;
  try {
    const auto names = storeNames();
    const auto project = projectJson();
    if (!before.renders.empty() && !names.renders.empty())
      RenderStore::keepFor(projectLocalFolder(project, "render-ahead"),
                           before.renders, names.renders);
    if (!before.facts.empty() && !names.facts.empty())
      RenderStore::keepFor(projectLocalFolder(project, "listing-facts"),
                           before.facts, names.facts);
  } catch (const std::exception &) {
    // Later sessions render again.
  }
}

RenderStore *Engine::storeOfViews() {
  if (!renderStoreMode_.empty())
    return liveRenderStore();
  const auto generation = listing().cacheGeneration();
  if (!rendersStoreRead_ || rendersStoreGeneration_ != generation) {
    rendersStoreRead_ = true;
    rendersStoreGeneration_ = generation;
    try {
      const auto name = storeNames().renders;
      if (name.empty()) {
        renderStore_.reset();
        rendersStoreName_.clear();
      } else if (name != rendersStoreName_) {
        renderStore_ = std::make_unique<RenderStore>(
            projectLocalFolder(projectJson(), "render-ahead"), name, false);
        rendersStoreName_ = name;
      }
    } catch (const std::exception &) {
      renderStore_.reset();
      rendersStoreName_.clear();
    }
  }
  return renderStore_.get();
}

Json Engine::keptRendersJson() {
  const auto *store = storeOfViews();
  return {{"kept", store ? store->records() : 0},
          {"complete", store && store->contains(CompleteKey)}};
}

void Engine::openFactsStore(const std::string &name, bool writable) {
  factsStore_ = std::make_unique<RenderStore>(
      projectLocalFolder(projectJson(), "listing-facts"), name, writable);
  factsStoreName_ = name;
}

std::vector<std::string> Engine::keptListingFacts(const std::string &key) {
  namespace limits = ::neverd::limits::worker;
  // A restored store follows its job.  An owner's follows what the identity
  // reads, which it cannot tell has changed before reading it.
  if (renderStoreMode_.empty() &&
      (!factsStoreRead_ ||
       factsStoreGeneration_ != listing_->cacheGeneration())) {
    factsStoreRead_ = true;
    factsStoreGeneration_ = listing_->cacheGeneration();
    try {
      const auto name = storeNames().facts;
      if (name.empty())
        factsStore_.reset();
      else if (name != factsStoreName_)
        openFactsStore(name, !readOnly_);
      if (name.empty())
        factsStoreName_.clear();
    } catch (const std::exception &) {
      factsStore_.reset();
      factsStoreName_.clear();
    }
  }
  if (!factsStore_)
    return {};
  auto compact = factsStore_->read(key);
  if (compact.empty() && renderStoreMode_ != "write") {
    const auto now = std::chrono::steady_clock::now();
    if (now - factsRefreshed_ >=
        std::chrono::milliseconds(limits::kListingFactsRefreshMilliseconds)) {
      factsRefreshed_ = now;
      factsStore_->refresh();
      compact = factsStore_->read(key);
    }
  }
  std::vector<std::string> pages;
  pages.reserve(compact.size());
  for (const auto &page : compact) {
    auto raw = decompressPage(page);
    if (!raw) {
      pages.clear();
      break;
    }
    pages.push_back(std::move(*raw));
  }
  ++(pages.empty() ? factsMisses_ : factsHits_);
  return pages;
}

void Engine::keepListingFact(const std::string &key, std::string bytes) {
  namespace limits = ::neverd::limits::worker;
  if (factsStore_ && factsStore_->writable() && !factsStore_->contains(key) &&
      factsStore_->bytes() < limits::kListingFactsMaxStoredBytes)
    factsStore_->write(key, std::vector<std::string>{compressPage(bytes)});
}

void Engine::keepListingFacts(std::uint64_t address) {
  namespace limits = ::neverd::limits::worker;
  if (!liveRenderStore() || !factsStore_ || !factsStore_->writable() ||
      factsStore_->bytes() >= limits::kListingFactsMaxStoredBytes)
    return;
  try {
    for (const auto &[key, pages] :
         listing().factsToKeep(address, [&](const std::string &candidate) {
           return !factsStore_->contains(candidate);
         })) {
      std::vector<std::string> compact;
      compact.reserve(pages.size());
      for (const auto &page : pages)
        compact.push_back(compressPage(page));
      factsStore_->write(key, compact);
    }
    if (const auto key = graphFactsKey(address); !factsStore_->contains(key)) {
      const auto text = backendText(neverd_cfg_json(session_, address), true);
      if (validFunctionGraph(Json::parse(text, nullptr, false)))
        factsStore_->write(key, std::vector<std::string>{compressPage(text)});
    }
  } catch (const std::exception &) {
    // The first look reports what the engine refuses.
  }
}

void Engine::restartRenderAhead() {
  if (renderAheadCursor_)
    renderStoreStale_ = true;
  renderedAhead_.clear();
  renderAheadOrder_.clear();
  renderAheadOrdered_ = false;
  renderAheadCursor_ = 0;
  renderAheadBytes_ = renderAheadRawBytes_ = renderAheadHeldBytes_ = 0;
  renderAheadRendered_ = renderAheadFailed_ = 0;
  const auto *store = liveRenderStore();
  renderAheadState_ =
      !renderAheadRequested_ ? RenderAheadState::Off
      : ++renderAheadStarts_ >
                  ::neverd::limits::worker::kRenderAheadMaxStarts ||
              (store && store->contains(CompleteKey) && !renderAheadVerifying_)
          ? RenderAheadState::Done
          : RenderAheadState::Pending;
}

bool Engine::rendersSettled() const {
  // An image the engine solves no summaries for reads the same from here on.
  if (summaryState_ == SummaryState::Unavailable)
    return true;
  // Preparing or solving summaries changes what views read; reading ahead
  // only shortens renders.  Both come first.
  return summaryState_ == SummaryState::Ready &&
         (prefillState_ == PrefillState::Done ||
          prefillState_ == PrefillState::Unavailable);
}

bool Engine::renderAheadPending() const {
  return renderAheadState_ == RenderAheadState::Pending && rendersSettled();
}

bool Engine::renderAheadStep() {
  namespace limits = ::neverd::limits::worker;
  const auto idle = std::chrono::steady_clock::now() - lastRequest_;
  if (!renderAheadOrdered_) {
    if (idle < holdOff(0))
      return false;
    // A visit would build the listing otherwise, and its new generation would
    // forget the renders.
    listing().build();
  }
  syncNamedViews();
  // A whole-program analysis answers every function from one pipeline.
  if (analyzed_ || !renderAheadPending()) {
    if (analyzed_)
      renderAheadState_ = RenderAheadState::Done;
    return true;
  }
  if (!renderAheadOrdered_) {
    const int count = neverd_func_count(session_);
    renderAheadOrder_.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
      // A function of a size the engine does not know could be any size.
      const auto size = static_cast<std::uint64_t>(
          std::max(neverd_func_size(session_, index), 0));
      if (size && size <= limits::kRenderAheadMaxFunctionBytes)
        renderAheadOrder_.emplace_back(size,
                                       neverd_func_entry(session_, index));
    }
    std::sort(renderAheadOrder_.begin(), renderAheadOrder_.end());
    renderAheadOrdered_ = true;
    return true;
  }
  auto *store = liveRenderStore();
  if (renderAheadCursor_ >= renderAheadOrder_.size()) {
    renderAheadState_ = RenderAheadState::Done;
    renderAheadVerifying_ = false;
    if (store) {
      store->write(CompleteKey, {});
      store->writeIndex();
    }
    if (factsStore_)
      factsStore_->writeIndex();
    return true;
  }
  if (renderAheadHeldBytes_ + (store ? store->bytes() : 0) >=
      limits::kRenderAheadMaxStoredBytes) {
    renderAheadState_ = RenderAheadState::Full;
    return true;
  }
  const auto [size, address] = renderAheadOrder_[renderAheadCursor_];
  if (idle < holdOff(size))
    return false;
  if (preparedFunction_ && preparedFunction_ == requestPrepared_ &&
      requestPrepareTime_ > std::chrono::milliseconds(
                                limits::kRenderAheadKeepPreparedMilliseconds) &&
      idle < std::chrono::milliseconds(
                 limits::kRenderAheadKeepPreparedIdleMilliseconds))
    return false;
  ++renderAheadCursor_;
  renderFunctionAhead(address);
  keepListingFacts(address);
  return true;
}

void Engine::renderFunctionAhead(std::uint64_t address) {
  namespace limits = ::neverd::limits::worker;
  const std::string representation = "source";
  Json stale = Json::array();
  const auto engineOptions =
      namedViewEngineOptions(address, representation, Json::object(), nullptr,
                             nullptr, nullptr, stale);
  const auto optionText = engineOptions.dump();
  // Read before preparing, as namedView reads it: preparing can change the
  // listing, and syncNamedViews then starts the job over.
  const auto sourceKey =
      namedViewSourceKey(address, representation, optionText);
  auto *store = liveRenderStore();
  const auto keptKey = store
                           ? renderStoreKey(address, representation, optionText)
                           : std::string();
  // A kept document answers a visit already, but not one in a later session.
  if (renderedAhead_.contains(sourceKey) ||
      (!store && namedViewsBySource_.contains(sourceKey)))
    return;
  if (store && store->contains(keptKey)) {
    if (keptRenderHolds(*store, keptKey))
      return;
    // What it read changed.  Rendering reads the summaries, which come first.
    if (summariesDeferred_) {
      summariesDeferred_ = false;
      restartSummaries();
      --renderAheadCursor_;
      return;
    }
  }
  // Later sessions leave what this one failed to render to their visits.
  const auto failed = [&] {
    ++renderAheadFailed_;
    if (store)
      store->write(keptKey, {});
  };
  const auto generation = listing().viewGeneration();
  std::vector<std::string> pages;
  std::size_t raw = 0, compact = 0, textBytes = 0;
  try {
    renderingAhead_ = true;
    struct EndRendering {
      bool &flag;
      ~EndRendering() { flag = false; }
    } end{renderingAhead_};
    prepareFunction(address, representation);
    if (listing().viewGeneration() != generation)
      return;
    for (std::size_t next = 0;;) {
      auto text = engineViewPage(address, representation, next, true,
                                 engineOptions, optionText);
      // What namedView asks of each page; one that fails it builds no
      // document, so the visit asks the engine and reports what that says.
      const auto page = parsedBackendText(text);
      if (!page.is_object() || !page.contains("text") ||
          !page["text"].is_string() || !page.contains("rows") ||
          !page["rows"].is_array() ||
          page.value("offset", std::size_t{0}) != next) {
        failed();
        return;
      }
      textBytes += page["text"].get_ref<const std::string &>().size();
      raw += text.size();
      pages.push_back(compressPage(text));
      compact += pages.back().size();
      if (page.value("complete", false))
        break;
      const Json following = page.value("next_offset", Json());
      if (!following.is_number_unsigned() ||
          following.get<std::size_t>() <= next ||
          textBytes > limits::kEngineMaxNamedViewBytes) {
        failed();
        return;
      }
      next = following.get<std::size_t>();
    }
  } catch (const std::exception &) {
    // The visit reports what the engine refused.
    failed();
    return;
  }
  renderAheadBytes_ += compact;
  renderAheadRawBytes_ += raw;
  ++renderAheadRendered_;
  pages.insert(pages.begin(), renderReads());
  if (!store || !store->write(keptKey, pages)) {
    renderAheadHeldBytes_ += compact;
    renderedAhead_.emplace(sourceKey, std::move(pages));
  }
}

void Engine::keepVisitedRender(std::uint64_t address,
                               const std::string &representation,
                               const std::string &optionText,
                               const std::vector<std::string> &pages) {
  auto *store = liveRenderStore();
  if (!store || !store->writable() || !rendersSettled() ||
      store->bytes() >= ::neverd::limits::worker::kRenderAheadMaxStoredBytes)
    return;
  const auto key = renderStoreKey(address, representation, optionText);
  if (store->contains(key) && keptRenderHolds(*store, key))
    return;
  std::vector<std::string> compact;
  compact.reserve(pages.size() + 1);
  compact.push_back(renderReads());
  for (const auto &page : pages)
    compact.push_back(compressPage(page));
  store->write(key, compact);
}

std::vector<std::string> Engine::renderedAheadPages(
    std::uint64_t address, const std::string &representation,
    const std::string &optionText, const std::string &sourceKey) {
  std::vector<std::string> pages, stored;
  renderFailedAddress_.reset();
  const auto kept = renderedAhead_.find(sourceKey);
  if (kept == renderedAhead_.end()) {
    // An owner reads the store only for a request that asks for that.
    if (auto *store =
            renderStoreMode_.empty() && !keptOnly_ ? nullptr : storeOfViews()) {
      const auto key = renderStoreKey(address, representation, optionText);
      stored = store->read(key);
      if (stored.empty() && store->contains(key))
        renderFailedAddress_ = address;
      if ((stored.empty() || !renderReadsHold(stored.front())) &&
          renderStoreMode_.empty()) {
        // What the replica has rendered since.
        const auto now = std::chrono::steady_clock::now();
        if (now - rendersRefreshed_ >=
            std::chrono::milliseconds(
                ::neverd::limits::worker::kListingFactsRefreshMilliseconds)) {
          rendersRefreshed_ = now;
          store->refresh();
          stored = store->read(key);
        }
      }
    }
    if (stored.empty())
      return pages;
  }
  const auto &compactPages =
      kept != renderedAhead_.end() ? kept->second : stored;
  // Its first says what the render read, and it answers while that holds.
  if (compactPages.empty() || !renderReadsHold(compactPages.front()))
    return pages;
  pages.reserve(compactPages.size() - 1);
  for (std::size_t at = 1; at < compactPages.size(); ++at) {
    const auto &compact = compactPages[at];
    auto page = decompressPage(compact);
    if (!page)
      return {};
    pages.push_back(std::move(*page));
  }
  ++renderAheadHits_;
  return pages;
}

Json Engine::renderAheadJson() const {
  static constexpr const char *Names[] = {"off", "pending", "done", "full"};
  return {{"state", Names[static_cast<int>(renderAheadState_)]},
          {"total", renderAheadOrder_.size()},
          {"rendered", renderAheadRendered_},
          {"failed", renderAheadFailed_},
          {"hits", renderAheadHits_},
          {"raw_bytes", renderAheadRawBytes_},
          {"bytes", renderAheadBytes_},
          {"kept", renderStore_ ? renderStore_->records() : 0},
          {"facts", listingFactsJson()}};
}

Json Engine::listingFactsJson() const {
  return {{"kept", factsStore_ ? factsStore_->records() : 0},
          {"bytes", factsStore_ ? factsStore_->bytes() : 0},
          {"hits", factsHits_},
          {"misses", factsMisses_}};
}

} // namespace neverd::worker
