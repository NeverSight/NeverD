//===- Executor.h - Headless plugin control queue --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A caller-driven serial queue for plugin control and completion messages.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_EXECUTOR_H
#define NEVERD_PLUGIN_EXECUTOR_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace neverd::plugin {

class Executor {
  std::mutex Mutex;
  std::condition_variable Ready;
  std::deque<std::function<void()>> Queue;

public:
  void post(std::function<void()> Work);
  /// Execute at most one queued item on the calling control thread.
  bool runOne(std::chrono::milliseconds Wait);
  void drain();
};

} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_EXECUTOR_H
