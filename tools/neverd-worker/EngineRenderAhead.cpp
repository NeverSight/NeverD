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
#include "CodeEdits.h"
#include "Engine.h"
#include "Listing.h"
#include "PageCodec.h"
#include "RenderStore.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
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

/// A view's key in the store: its source key without the generation, which
/// counts this session's listing builds.
std::string storeKey(std::uint64_t address, const std::string &representation,
                     const std::string &optionText) {
  return hexAddress(address) + ":" + representation + ":" + optionText;
}

/// Kept once a job has reached every function.
const std::string CompleteKey = "complete";
} // namespace

Json Engine::renderStoreIdentity() {
  auto identity = browseCacheIdentity();
  auto &state = identity.at("state");
  state["code_edits"] =
      CodeEdits::analysisState(state.value("code_edits", Json::array()));
  identity.at("shared").erase(
      std::string(project_files::describe(project_files::File::Code).Path));
  return identity;
}

void Engine::openRenderStore(const std::string &mode) {
  renderStore_.reset();
  renderStoreStale_ = false;
  summariesDeferred_ = false;
  if (mode.empty())
    return;
  if (mode != "read" && mode != "write")
    throw Error("invalid_request", "render_store is read or write");
  const auto project = projectJson();
  if (!browseCacheEnabled_ || !project.is_object() ||
      !project.contains("folder"))
    return;
  try {
    const auto name = BrowseCache::digest(BrowseCache::runtimeIdentity() +
                                          renderStoreIdentity().dump());
    if (name.empty())
      return;
    const auto &folder = project.at("folder").get_ref<const std::string &>();
    renderStore_ = std::make_unique<RenderStore>(
        std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t *>(folder.data()), folder.size())) /
            "local" / "render-ahead",
        name, mode == "write");
  } catch (const std::exception &) {
    // The job keeps what it renders in memory, and later sessions render it
    // again.
    return;
  }
  summariesDeferred_ = summaryJob_ && renderStore_->contains(CompleteKey);
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
              (store && store->contains(CompleteKey))
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
    if (store)
      store->write(CompleteKey, {});
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
  const auto keptKey =
      store ? storeKey(address, representation, optionText) : std::string();
  // A kept document answers a visit already, but not one in a later session.
  if (renderedAhead_.contains(sourceKey) ||
      (store ? store->contains(keptKey)
             : namedViewsBySource_.contains(sourceKey)))
    return;
  // Later sessions leave what this one failed to render to their visits.
  const auto failed = [&] {
    ++renderAheadFailed_;
    if (store)
      store->write(keptKey, {});
  };
  const auto generation = listing().generation();
  std::vector<std::string> pages;
  std::size_t raw = 0, compact = 0, textBytes = 0;
  try {
    renderingAhead_ = true;
    struct EndRendering {
      bool &flag;
      ~EndRendering() { flag = false; }
    } end{renderingAhead_};
    prepareFunction(address, representation);
    if (listing().generation() != generation)
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
  const auto key = storeKey(address, representation, optionText);
  if (store->contains(key))
    return;
  std::vector<std::string> compact;
  compact.reserve(pages.size());
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
    if (auto *store = liveRenderStore()) {
      const auto key = storeKey(address, representation, optionText);
      stored = store->read(key);
      if (stored.empty() && store->contains(key))
        renderFailedAddress_ = address;
    }
    if (stored.empty())
      return pages;
  }
  const auto &compactPages =
      kept != renderedAhead_.end() ? kept->second : stored;
  pages.reserve(compactPages.size());
  for (const auto &compact : compactPages) {
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
          {"kept", renderStore_ ? renderStore_->records() : 0}};
}

} // namespace neverd::worker
