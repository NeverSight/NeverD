//===- Parallel.cpp - Large-stack task scheduling
//-----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// oneTBB scheduling with a synchronous native coordinator and checked stacks.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/Parallel.h"

#if NEVERD_USE_TBB
#include "oneapi/tbb/global_control.h"
#include "oneapi/tbb/task_arena.h"
#include "oneapi/tbb/task_group.h"

#include <stdexcept>

namespace neverd {
namespace {

thread_local bool InPipelineTask = false;

struct SchedulerLifetime {
  oneapi::tbb::global_control Stack{
      oneapi::tbb::global_control::thread_stack_size,
      kDecompilerWorkerStackSize};
  oneapi::tbb::task_scheduler_handle Retain{oneapi::tbb::attach{}};
};

bool hasWorkerStack() {
#ifdef _WIN32
  // VirtualQuery finds the reservation base, including uncommitted pages.
  // The current stack limits alone may describe only the committed range.
  MEMORY_BASIC_INFORMATION Info{};
  ULONG_PTR Low = 0, High = 0;
  GetCurrentThreadStackLimits(&Low, &High);
  if (!VirtualQuery(reinterpret_cast<const void *>(Low), &Info, sizeof(Info)) ||
      !Info.AllocationBase)
    return false;
  const auto Base = reinterpret_cast<uintptr_t>(Info.AllocationBase);
  return High >= Base && High - Base >= kDecompilerWorkerStackSize;
#elif defined(__APPLE__)
  return pthread_get_stacksize_np(pthread_self()) >= kDecompilerWorkerStackSize;
#elif defined(__linux__)
  pthread_attr_t Attr;
  if (pthread_getattr_np(pthread_self(), &Attr) != 0)
    return false;
  size_t Size = 0;
  const int Result = pthread_attr_getstacksize(&Attr, &Size);
  pthread_attr_destroy(&Attr);
  return Result == 0 && Size >= kDecompilerWorkerStackSize;
#else
  return false;
#endif
}

bool hasSpareWorkerStack() {
  const char Marker = 0;
  const auto Cursor = reinterpret_cast<uintptr_t>(&Marker);
  uintptr_t Base = 0;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION Info{};
  if (!VirtualQuery(&Marker, &Info, sizeof(Info)) || !Info.AllocationBase)
    return false;
  Base = reinterpret_cast<uintptr_t>(Info.AllocationBase);
#elif defined(__APPLE__)
  const auto High =
      reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
  const auto Size = pthread_get_stacksize_np(pthread_self());
  if (High < Size)
    return false;
  Base = High - Size;
#elif defined(__linux__)
  pthread_attr_t Attr;
  if (pthread_getattr_np(pthread_self(), &Attr) != 0)
    return false;
  void *Address = nullptr;
  size_t Size = 0;
  const int Result = pthread_attr_getstack(&Attr, &Address, &Size);
  pthread_attr_destroy(&Attr);
  if (Result != 0)
    return false;
  Base = reinterpret_cast<uintptr_t>(Address);
#else
  return false;
#endif
  // These supported hosts grow stacks downward. Reserve another full worker
  // stack below the caller's live frames, plus room for scheduler frames.
  return Cursor > Base &&
         Cursor - Base >= kDecompilerWorkerStackSize + 64 * 1024;
}

struct TaskScope {
  bool Previous = InPipelineTask;
  TaskScope() { InPipelineTask = true; }
  ~TaskScope() { InPipelineTask = Previous; }
};

} // namespace

void runTaskWorkers(unsigned Count, void (*Body)(void *), void *Context) {
  if (!Count)
    return;
  if (InPipelineTask) {
    // The outer callback may retain a large frame. Drain nested work on a
    // fresh native stack without waiting for occupied TBB workers.
    const auto Caller = std::this_thread::get_id();
    auto Nested = [&] {
      if (std::this_thread::get_id() == Caller || !hasWorkerStack())
        throw std::runtime_error("cannot allocate a fresh nested worker stack");
      TaskScope Scope;
      Body(Context);
    };
    runWithLargeStackThreads(1, Nested);
    return;
  }
  const bool ReuseCaller = hasSpareWorkerStack();
  if (Count == 1 && ReuseCaller) {
    // A serial outer loop occupies no pool worker. Its independent inner
    // batches can use the scheduler, which checks their remaining headroom.
    Body(Context);
    return;
  }
  // Keep workers available between synchronous pipeline phases.
  static SchedulerLifetime Lifetime;
  auto Coordinate = [&] {
    if (!hasWorkerStack())
      throw std::runtime_error("cannot allocate a large pipeline worker stack");
    const unsigned Limit =
        std::min<unsigned>(Count, std::numeric_limits<int>::max());
    std::vector<std::exception_ptr> Failures(Limit);
    oneapi::tbb::task_arena Arena(static_cast<int>(Limit));
    Arena.execute([&] {
      oneapi::tbb::task_group_context Isolation(
          oneapi::tbb::task_group_context::isolated);
      oneapi::tbb::task_group Tasks(Isolation);
      std::exception_ptr SubmissionFailure;
      for (unsigned I = 0; I < Limit; ++I) {
        try {
          Tasks.run([&, I] {
            try {
              auto Invoke = [&] {
                if (!hasWorkerStack())
                  throw std::runtime_error(
                      "cannot allocate a large pipeline worker stack");
                TaskScope Scope;
                Body(Context);
              };
              if (hasWorkerStack())
                Invoke();
              else
                // A host may have created smaller TBB workers before us.
                // Invoke checks again if native creation falls back inline.
                runWithLargeStackThreads(1, Invoke);
            } catch (...) {
              Failures[I] = std::current_exception();
            }
          });
        } catch (...) {
          SubmissionFailure = std::current_exception();
          break;
        }
      }
      Tasks.wait();
      if (SubmissionFailure)
        std::rethrow_exception(SubmissionFailure);
    });
    for (const auto &Failure : Failures)
      if (Failure)
        std::rethrow_exception(Failure);
  };
  // The CLI already has a 128 MiB stack. Reuse that headroom; small SDK
  // callers still receive a fresh coordinator because wait() executes tasks.
  if (ReuseCaller)
    Coordinate();
  else
    runWithLargeStackThreads(1, Coordinate);
}
} // namespace neverd
#else
void neverd::runTaskWorkers(unsigned Count, void (*Body)(void *),
                            void *Context) {
  auto Worker = [&] { Body(Context); };
  runWithLargeStackThreads(Count, Worker);
}
#endif
