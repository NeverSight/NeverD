//===- SupportThreadTests.cpp - Thread completion and failure contracts
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/Parallel.h"
#include "neverd/support/StackSizeMain.h"

#include <array>
#include <atomic>
#include <barrier>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct WorkerFailure {
  unsigned Identity;
};

struct ScopedThreadCount {
  unsigned Previous = neverd::workerThreadOverride().load();
  explicit ScopedThreadCount(unsigned Count) {
    neverd::setWorkerThreadCount(Count);
  }
  ~ScopedThreadCount() { neverd::setWorkerThreadCount(Previous); }
};

int throwingMain(int, char *[]) { throw WorkerFailure{42}; }

int successfulMain(int Argc, char *Argv[]) {
  return Argc == 1 && std::string(Argv[0]) == "neverd" ? 17 : -1;
}

} // namespace

TEST(SupportThreads, ZeroWorkersDoNotInvokeBody) {
  auto Worker = [] { FAIL() << "zero workers must not invoke the body"; };
  neverd::runWithLargeStackThreads(0, Worker);
}

TEST(SupportThreads, PreservesExceptionTypeAndPayload) {
  auto Worker = [] { throw WorkerFailure{73}; };
  try {
    neverd::runWithLargeStackThreads(1, Worker);
    FAIL() << "worker exception was lost";
  } catch (const WorkerFailure &Failure) {
    EXPECT_EQ(Failure.Identity, 73u);
  }
}

TEST(SupportThreads, JoinsEveryWorkerBeforeRethrowing) {
  constexpr unsigned Count = 4;
  std::atomic<unsigned> Next{0};
  std::barrier Started(Count);
  std::array<unsigned, Count> Completed{};
  auto Worker = [&] {
    const unsigned Index = Next.fetch_add(1);
    struct Completion {
      unsigned &Slot;
      ~Completion() { Slot = 1; }
    } OnExit{Completed[Index]};
    Started.arrive_and_wait();
    if (Index == 0)
      throw WorkerFailure{Index};
  };
  EXPECT_THROW(neverd::runWithLargeStackThreads(Count, Worker), WorkerFailure);
  // These non-atomic writes are safe to observe only after all workers joined,
  // including stack unwinding in the worker that threw.
  EXPECT_EQ(Completed, (std::array<unsigned, Count>{1, 1, 1, 1}));
}

TEST(SupportThreads, ConcurrentFailuresPreserveOneOriginalException) {
  constexpr unsigned Count = 4;
  std::atomic<unsigned> Next{0};
  std::atomic<unsigned> Completed{0};
  std::barrier Started(Count);
  auto Worker = [&] {
    const unsigned Index = Next.fetch_add(1);
    struct Completion {
      std::atomic<unsigned> &Count;
      ~Completion() { Count.fetch_add(1); }
    } OnExit{Completed};
    Started.arrive_and_wait();
    throw WorkerFailure{Index};
  };
  try {
    neverd::runWithLargeStackThreads(Count, Worker);
    FAIL() << "concurrent worker exceptions were lost";
  } catch (const WorkerFailure &Failure) {
    EXPECT_LT(Failure.Identity, Count);
    EXPECT_EQ(Completed.load(), Count);
  }
}

TEST(SupportThreads, MainPreservesArgumentsAndResult) {
  char Name[] = "neverd";
  char *Argv[] = {Name, nullptr};
  EXPECT_EQ(neverd::runWithLargeStack(successfulMain, 1, Argv), 17);
}

TEST(SupportThreads, MainPreservesExceptionTypeAndPayload) {
  try {
    neverd::runWithLargeStack(throwingMain, 0, nullptr);
    FAIL() << "main exception was lost";
  } catch (const WorkerFailure &Failure) {
    EXPECT_EQ(Failure.Identity, 42u);
  }
}

TEST(SupportThreads, ParallelLoopVisitsEveryIndexOnce) {
  ScopedThreadCount Threads(4);
  std::array<std::atomic<unsigned>, 37> Visits{};
  neverd::parallelForEach(Visits.size(), [&](auto Claim, size_t Total) {
    for (size_t Index; (Index = Claim()) < Total;)
      Visits[Index].fetch_add(1);
  });
  for (size_t Index = 0; Index < Visits.size(); ++Index)
    EXPECT_EQ(Visits[Index].load(), 1u) << "index " << Index;
}

TEST(SupportThreads, ParallelLoopPropagatesWorkerFailure) {
  ScopedThreadCount Threads(2);
  EXPECT_THROW(neverd::parallelForEach(3,
                                       [](auto, size_t) {
                                         throw std::runtime_error(
                                             "parallel work failed");
                                       }),
               std::runtime_error);
}

TEST(SupportThreads, WeightedLoopRetainsDescendingStableDispatch) {
  ScopedThreadCount Threads(1);
  std::vector<size_t> Visited;
  neverd::parallelForEachWeighted(
      {5, 15, 15, 1}, [&](auto Claim, size_t Total) {
        for (size_t Index; (Index = Claim()) < Total;)
          Visited.push_back(Index);
      });
  EXPECT_EQ(Visited, (std::vector<size_t>{1, 2, 0, 3}));
}

TEST(SupportThreads, WeightedLoopPropagatesWorkerFailure) {
  ScopedThreadCount Threads(2);
  EXPECT_THROW(neverd::parallelForEachWeighted({5, 15, 1},
                                               [](auto, size_t) {
                                                 throw std::runtime_error(
                                                     "weighted work failed");
                                               }),
               std::runtime_error);
}

TEST(SupportThreads, EmptyLoopsDoNotInvokeBody) {
  auto Worker = [](auto, size_t) {
    FAIL() << "empty work must not invoke the body";
  };
  neverd::parallelForEach(0, Worker);
  neverd::parallelForEachWeighted({}, Worker);
}
