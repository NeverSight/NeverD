//===- SchedulerTests.cpp - Synchronous scheduling contracts
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verifies task bounds, nested queues, exception identity and large stacks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/Parallel.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#if NEVERD_USE_TBB
#include "oneapi/tbb/global_control.h"
#include "oneapi/tbb/task_arena.h"
#include "oneapi/tbb/task_group.h"
#endif

namespace {
struct Failure {
  unsigned Value;
};

void checkBatch(unsigned Limit) {
  std::array<std::atomic<unsigned>, 257> Visits{};
  std::atomic<unsigned> Next{0}, Active{0}, Peak{0};
  auto Work = [&] {
    const unsigned Now = Active.fetch_add(1) + 1;
    unsigned Old = Peak.load();
    while (Now > Old && !Peak.compare_exchange_weak(Old, Now)) {
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    for (unsigned I; (I = Next.fetch_add(1)) < Visits.size();) {
      Visits[I].fetch_add(1);
    }
    Active.fetch_sub(1);
  };
  neverd::runPipelineWorkers(Limit, Work);
  EXPECT_EQ(Active.load(), 0u);
  EXPECT_LE(Peak.load(), Limit);
  for (const auto &Count : Visits)
    EXPECT_EQ(Count.load(), 1u);
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
unsigned touchLargeStack() {
  volatile uint8_t Bytes[5 * 1024 * 1024];
  unsigned Sum = 0;
  for (size_t I = 0; I < sizeof(Bytes); I += 4096)
    Bytes[I] = static_cast<uint8_t>(I / 4096);
  for (size_t I = 0; I < sizeof(Bytes); I += 4096)
    Sum += Bytes[I];
  return Sum;
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
unsigned nestedLargeStack() {
  volatile uint8_t Bytes[5 * 1024 * 1024];
  for (size_t I = 0; I < sizeof(Bytes); I += 4096)
    Bytes[I] = static_cast<uint8_t>(I / 4096);
  auto Inner = [] { EXPECT_EQ(touchLargeStack(), 163200u); };
  neverd::runPipelineWorkers(2, Inner);
  unsigned Sum = 0;
  for (size_t I = 0; I < sizeof(Bytes); I += 4096)
    Sum += Bytes[I];
  return Sum;
}
} // namespace

#if NEVERD_USE_TBB
// Keep this first in this translation unit: the complete inventory starts a
// foreign arena before NeverD's first scheduler call, as an SDK host can do.
TEST(TaskScheduler, ForeignWorkersBeforeFirstBatch) {
  oneapi::tbb::global_control Stack(
      oneapi::tbb::global_control::thread_stack_size, 512 * 1024);
  oneapi::tbb::task_scheduler_handle Retain{oneapi::tbb::attach{}};
  oneapi::tbb::task_arena Arena(4);
  std::atomic<unsigned> Completed{0};
  Arena.execute([&] {
    oneapi::tbb::task_group Warm;
    for (unsigned I = 0; I < 4; ++I)
      Warm.run([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto Work = [&] {
          EXPECT_EQ(touchLargeStack(), 163200u);
          Completed.fetch_add(1);
        };
        neverd::runPipelineWorkers(4, Work);
      });
    Warm.wait();
  });
  EXPECT_EQ(Completed.load(), 16u);
}
#endif

TEST(TaskScheduler, CapsAndCompletesRepeatedAndConcurrentBatches) {
  for (unsigned Limit : {1u, 2u, 4u})
    for (unsigned I = 0; I < 8; ++I)
      checkBatch(Limit);
  std::thread A([] { checkBatch(2); });
  std::thread B([] { checkBatch(3); });
  A.join();
  B.join();
}

TEST(TaskScheduler, NestedQueuesFinish) {
  std::atomic<unsigned> Next{0}, Visited{0};
  auto Outer = [&] {
    for (unsigned I; (I = Next.fetch_add(1)) < 64;) {
      std::atomic<unsigned> InnerNext{0};
      auto Inner = [&] {
        for (unsigned J; (J = InnerNext.fetch_add(1)) < 64;)
          Visited.fetch_add(1);
      };
      neverd::runPipelineWorkers(3, Inner);
    }
  };
  neverd::runPipelineWorkers(4, Outer);
  EXPECT_EQ(Visited.load(), 4096u);
}

TEST(TaskScheduler, CompletesEveryWrapperBeforeRethrowingOriginal) {
  std::atomic<unsigned> Completed{0};
  auto Work = [&] {
    const auto I = Completed.fetch_add(1);
    throw Failure{I + 37};
  };
  try {
    neverd::runPipelineWorkers(4, Work);
    FAIL() << "exception was lost";
  } catch (const Failure &F) {
    EXPECT_GE(F.Value, 37u);
    EXPECT_LT(F.Value, 41u);
  }
  EXPECT_EQ(Completed.load(), 4u);
  checkBatch(4);
}

TEST(TaskScheduler, SmallCallerUsesLargeWorkerStacks) {
  std::atomic<unsigned> Completed{0};
  auto Caller = [&] {
    auto Work = [&] {
      EXPECT_EQ(touchLargeStack(), 163200u);
      Completed.fetch_add(1);
    };
    neverd::runPipelineWorkers(4, Work);
  };
  // std::thread has only a 512 KiB stack on macOS.
  std::thread Small(Caller);
  Small.join();
  EXPECT_EQ(Completed.load(), 4u);
}

TEST(TaskScheduler, SmallAndSerialBatchesRetainTheWorkerStackContract) {
  std::array<unsigned, 3> Completed{};
  std::thread Caller([&] {
    auto Work = [&](auto Claim, size_t Total) {
      for (size_t Index; (Index = Claim()) < Total;) {
        EXPECT_EQ(touchLargeStack(), 163200u);
        ++Completed[Index];
      }
    };
    neverd::parallelForEach(Completed.size(), Work);
    neverd::parallelForEachWeighted({5, 15, 1}, Work);
    // A caller may force serial work even above the batching threshold.
    neverd::parallelForEachWeighted({5, 15, 1}, Work, 1, 1);
  });
  Caller.join();
  EXPECT_EQ(Completed, (std::array<unsigned, 3>{3, 3, 3}));
}

TEST(TaskScheduler, NestedCallbacksRetainIndependentLargeFrames) {
  auto Work = [] { EXPECT_EQ(nestedLargeStack(), 163200u); };
  neverd::runPipelineWorkers(2, Work);
}

#if NEVERD_USE_TBB
TEST(TaskScheduler, LargeCallerReusesAvailableHeadroom) {
  const auto Main = [](int, char *[]) {
    const auto Caller = std::this_thread::get_id();
    bool SameThread = false;
    auto Work = [&] {
      SameThread = std::this_thread::get_id() == Caller;
      EXPECT_EQ(touchLargeStack(), 163200u);
    };
    neverd::runPipelineWorkers(1, Work);
    return SameThread ? 0 : 1;
  };
  EXPECT_EQ(neverd::runWithLargeStack(Main, 0, nullptr), 0);
}

TEST(TaskScheduler, ForeignSerialLimitStillCompletes) {
  oneapi::tbb::global_control Limit(
      oneapi::tbb::global_control::max_allowed_parallelism, 1);
  checkBatch(4);
}
#endif
