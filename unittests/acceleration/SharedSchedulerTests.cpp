//===- SharedSchedulerTests.cpp - Shared scheduler ownership
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercises synchronous tasks and original exceptions across library links.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/Parallel.h"

#include <array>
#include <atomic>
#include <thread>

extern "C" {
void ownerRun(unsigned Count, void (*Body)(void *), void *Context);
void pluginRun(unsigned Count, void (*Body)(void *), void *Context);
void componentRun(unsigned Count, void (*Body)(void *), void *Context);
}

namespace {
constexpr std::array Entries{&ownerRun, &pluginRun, &componentRun,
                             &neverd::runTaskWorkers};
constexpr unsigned WorkerCount = 4;
struct Failure {
  unsigned Value;
};
} // namespace

TEST(SharedTaskScheduler, CompletesCallbacksFromEveryLinkBoundary) {
  for (auto Entry : Entries) {
    std::atomic<unsigned> Completed{0};
    Entry(
        WorkerCount,
        [](void *Context) {
          std::this_thread::yield();
          static_cast<std::atomic<unsigned> *>(Context)->fetch_add(1);
        },
        &Completed);
    EXPECT_EQ(Completed.load(), WorkerCount);
  }
}

TEST(SharedTaskScheduler, JoinsCallbacksBeforeRethrowingOriginalException) {
  for (auto Entry : Entries) {
    std::atomic<unsigned> Completed{0};
    try {
      Entry(
          WorkerCount,
          [](void *Context) {
            auto &Count = *static_cast<std::atomic<unsigned> *>(Context);
            throw Failure{Count.fetch_add(1)};
          },
          &Completed);
      FAIL() << "exception was lost at a shared scheduler boundary";
    } catch (const Failure &Error) {
      EXPECT_LT(Error.Value, WorkerCount);
    }
    EXPECT_EQ(Completed.load(), WorkerCount);
  }
}
