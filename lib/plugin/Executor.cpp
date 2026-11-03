//===- Executor.cpp - Headless plugin control queue -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A caller-driven serial queue for plugin control and completion messages.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Executor.h"

#include <utility>

namespace neverd::plugin {

void Executor::post(std::function<void()> Work) {
  {
    std::lock_guard Lock(Mutex);
    Queue.push_back(std::move(Work));
  }
  Ready.notify_one();
}

bool Executor::runOne(std::chrono::milliseconds Wait) {
  std::function<void()> Work;
  {
    std::unique_lock Lock(Mutex);
    if (!Ready.wait_for(Lock, Wait, [&] { return !Queue.empty(); }))
      return false;
    Work = std::move(Queue.front());
    Queue.pop_front();
  }
  Work();
  return true;
}

void Executor::drain() {
  while (runOne(std::chrono::milliseconds::zero())) {
  }
}

} // namespace neverd::plugin
