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
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "Listing.h"
#include "PageCodec.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"

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
} // namespace

void Engine::restartRenderAhead() {
  renderedAhead_.clear();
  renderAheadOrder_.clear();
  renderAheadOrdered_ = false;
  renderAheadCursor_ = 0;
  renderAheadBytes_ = renderAheadRawBytes_ = 0;
  renderAheadRendered_ = renderAheadFailed_ = 0;
  renderAheadState_ =
      !renderAheadRequested_ ? RenderAheadState::Off
      : ++renderAheadStarts_ > ::neverd::limits::worker::kRenderAheadMaxStarts
          ? RenderAheadState::Done
          : RenderAheadState::Pending;
}

bool Engine::renderAheadPending() const {
  if (renderAheadState_ != RenderAheadState::Pending)
    return false;
  // An image the engine solves no summaries for reads the same from here on.
  if (summaryState_ == SummaryState::Unavailable)
    return true;
  // Preparing or solving summaries changes what views read; reading ahead
  // only shortens renders.  Both come first.
  return summaryState_ == SummaryState::Ready &&
         (prefillState_ == PrefillState::Done ||
          prefillState_ == PrefillState::Unavailable);
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
  if (renderAheadCursor_ >= renderAheadOrder_.size()) {
    renderAheadState_ = RenderAheadState::Done;
    return true;
  }
  if (renderAheadBytes_ >= limits::kRenderAheadMaxStoredBytes) {
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
  // A kept document answers a visit already.
  if (namedViewsBySource_.contains(sourceKey) ||
      renderedAhead_.contains(sourceKey))
    return;
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
        ++renderAheadFailed_;
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
        ++renderAheadFailed_;
        return;
      }
      next = following.get<std::size_t>();
    }
  } catch (const std::exception &) {
    // The visit reports what the engine refused.
    ++renderAheadFailed_;
    return;
  }
  renderAheadBytes_ += compact;
  renderAheadRawBytes_ += raw;
  ++renderAheadRendered_;
  renderedAhead_.emplace(sourceKey, std::move(pages));
}

std::vector<std::string>
Engine::renderedAheadPages(const std::string &sourceKey) {
  std::vector<std::string> pages;
  const auto kept = renderedAhead_.find(sourceKey);
  if (kept == renderedAhead_.end())
    return pages;
  pages.reserve(kept->second.size());
  for (const auto &compact : kept->second) {
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
          {"bytes", renderAheadBytes_}};
}

} // namespace neverd::worker
