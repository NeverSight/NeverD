//===- DeferredRelease.h - Off-request object release -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A thread that destroys objects a request retires, such as a cached view
/// document evicted for a new one, so the request answers without waiting
/// for their memory to be freed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_DEFERREDRELEASE_H
#define NEVERD_TOOLS_NEVERD_WORKER_DEFERREDRELEASE_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace neverd::worker {

/// Drops references on its own thread, started on the first retire.  Only
/// the references move: what an object owns must be safe to destroy on
/// another thread, as a self-contained document is.  Destroying the
/// releaser joins the thread after it has dropped every retired reference.
class DeferredRelease {
public:
  DeferredRelease() = default;
  DeferredRelease(const DeferredRelease &) = delete;
  DeferredRelease &operator=(const DeferredRelease &) = delete;
  ~DeferredRelease();

  /// Hands \p object's reference to the releasing thread.  Where that
  /// thread cannot take it, the reference is dropped here instead.
  void retire(std::shared_ptr<const void> object) noexcept;

private:
  void run();

  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::shared_ptr<const void>> pending_;
  bool stopping_ = false;
  std::thread thread_;
};

} // namespace neverd::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_DEFERREDRELEASE_H
