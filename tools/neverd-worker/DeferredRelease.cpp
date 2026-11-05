//===- DeferredRelease.cpp - Off-request object release -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The releasing thread behind DeferredRelease.
///
//===----------------------------------------------------------------------===//

#include "DeferredRelease.h"

#include <utility>

namespace neverd::worker {

DeferredRelease::~DeferredRelease() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_one();
  if (thread_.joinable())
    thread_.join();
}

void DeferredRelease::retire(std::shared_ptr<const void> object) noexcept {
  if (!object)
    return;
  try {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      if (!thread_.joinable())
        thread_ = std::thread([this] { run(); });
      pending_.push_back(std::move(object));
    }
    changed_.notify_one();
  } catch (...) {
    // No thread or no room for the reference: it goes with this frame.
  }
}

void DeferredRelease::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    changed_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
    if (pending_.empty())
      return;
    std::vector<std::shared_ptr<const void>> released;
    released.swap(pending_);
    lock.unlock();
    released.clear();
    lock.lock();
  }
}

} // namespace neverd::worker
