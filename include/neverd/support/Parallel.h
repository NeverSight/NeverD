//===- Parallel.h - Lightweight parallel helpers ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Work-stealing parallel loop utilities used across pipeline phases.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_PARALLEL_H
#define NEVERD_SUPPORT_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace neverd {

/// Preferred default worker-thread count for a fresh process.
///
/// On a homogeneous machine this is simply the logical-core count, which lets
/// the parallel IR phases scale all the way up on a many-core server (the
/// historical hard cap of 12 left those cores idle).  On a heterogeneous Apple
/// Silicon host the efficiency cores run the same work perhaps 2-3x slower, so
/// on the short latency-bound phases their stragglers cost more wall time than
/// the throughput they add; there the performance-core count
/// (hw.perflevel0.logicalcpu) is the better default.  Intel Macs and every
/// non-Apple platform lack that sysctl and fall through to the full core count.
/// The value is only a default — NEVERD_THREADS or setWorkerThreadCount() can
/// push it higher (e.g. to also use the efficiency cores) or lower.
inline unsigned defaultWorkerThreadCount() {
  const unsigned Hw = std::max(1u, std::thread::hardware_concurrency());
#if defined(__APPLE__)
  int32_t PerfCores = 0;
  size_t Sz = sizeof(PerfCores);
  if (sysctlbyname("hw.perflevel0.logicalcpu", &PerfCores, &Sz, nullptr, 0) ==
          0 &&
      PerfCores > 0 && static_cast<unsigned>(PerfCores) <= Hw)
    return static_cast<unsigned>(PerfCores);
#endif
  return Hw;
}

/// Programmatic override for the IR-phase worker-thread count.  0 = auto
/// (use the environment variable, else the hardware concurrency).  Shared
/// across translation units via the inline definition.
inline std::atomic<unsigned> &workerThreadOverride() {
  static std::atomic<unsigned> Override{0};
  return Override;
}

/// Set the number of worker threads used by the parallel IR phases.
/// Pass 0 to restore automatic selection.
inline void setWorkerThreadCount(unsigned N) {
  workerThreadOverride().store(N, std::memory_order_relaxed);
}

/// Number of worker threads for the parallel IR phases (function decode,
/// Low->Med, Med->High).  Resolution order:
///   1. an explicit setWorkerThreadCount() override, else
///   2. the NEVERD_THREADS environment variable (parsed once), else
///   3. std::thread::hardware_concurrency().
///
/// Historically this was capped at 12, which idled cores on hosts with more
/// than twelve of them — the LLVM emission phase already spawns one thread per
/// core and deliberately bypassed that cap.  The IR phases write only to
/// per-index result slots (no shared-state contention), so they scale with the
/// core count just as well; the cap is gone and the default is every core.
inline unsigned workerThreadCount() {
  if (unsigned O = workerThreadOverride().load(std::memory_order_relaxed))
    return O;

  static const unsigned EnvThreads = [] {
    if (const char *E = std::getenv("NEVERD_THREADS")) {
      unsigned V = 0;
      const char *End = E + std::strlen(E);
      auto Result = std::from_chars(E, End, V, 10);
      if (Result.ec == std::errc() && Result.ptr == End && V > 0)
        return V;
    }
    return 0u;
  }();
  if (EnvThreads)
    return EnvThreads;

  return defaultWorkerThreadCount();
}

/// Parallel work-stealing loop.  Spawns \c workerThreadCount() threads,
/// each invoking \p ThreadBody(Claim, Total).  \c Claim() returns the next
/// unprocessed index; indices >= \p Total signal exhaustion.
template <typename Fn> void parallelForEach(size_t Total, Fn ThreadBody) {
  if (Total == 0)
    return;
  const unsigned NumThreads = static_cast<unsigned>(
      std::min<size_t>(workerThreadCount(), Total));
  std::atomic<size_t> NextIdx{0};
  auto Claim = [&]() -> size_t {
    return NextIdx.fetch_add(1, std::memory_order_relaxed);
  };
  std::vector<std::thread> Threads;
  Threads.reserve(NumThreads);
  for (unsigned T = 0; T < NumThreads; ++T)
    Threads.emplace_back([&] { ThreadBody(Claim, Total); });
  for (auto &T : Threads)
    T.join();
}

/// Parallel work-stealing loop that hands out indices heaviest-first.
///
/// Behaves exactly like \c parallelForEach — \p ThreadBody receives the same
/// \c (Claim, Total) pair and \c Claim() returns the next real element index —
/// except indices are dispatched in order of descending \p Weight rather than
/// 0,1,2,...  Dynamic atomic claiming already balances most of the work, but a
/// single very large element claimed last serializes the tail while every other
/// thread idles.  Processing the heaviest elements first (longest-processing-
/// time scheduling) keeps the tail short; \p Weight must have exactly \p Total
/// entries (a cheap per-element cost proxy such as an op count).
template <typename Fn>
void parallelForEachWeighted(const std::vector<uint64_t> &Weight,
                             Fn ThreadBody) {
  const size_t Total = Weight.size();
  if (Total == 0)
    return;
  std::vector<size_t> Order(Total);
  for (size_t I = 0; I < Total; ++I)
    Order[I] = I;
  std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
    return Weight[A] != Weight[B] ? Weight[A] > Weight[B] : A < B;
  });

  const unsigned NumThreads = static_cast<unsigned>(
      std::min<size_t>(workerThreadCount(), Total));
  std::atomic<size_t> NextPos{0};
  // Claim returns the next real element index (mapped through the heaviest-
  // first order), or a value >= Total once the work is exhausted — matching the
  // contract the existing thread bodies already rely on.
  auto Claim = [&]() -> size_t {
    size_t P = NextPos.fetch_add(1, std::memory_order_relaxed);
    return P < Total ? Order[P] : Total;
  };
  std::vector<std::thread> Threads;
  Threads.reserve(NumThreads);
  for (unsigned T = 0; T < NumThreads; ++T)
    Threads.emplace_back([&] { ThreadBody(Claim, Total); });
  for (auto &T : Threads)
    T.join();
}

} // namespace neverd

#endif // NEVERD_SUPPORT_PARALLEL_H
