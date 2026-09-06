//===- StackSizeMain.h - Large-stack thread helpers ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Provides helpers that run the main entry point and parallel pipeline work
/// on threads with larger stacks to accommodate deep decompiler recursion.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_STACKSIZEMAIN_H
#define NEVERD_SUPPORT_STACKSIZEMAIN_H

#include <cstddef>
#include <vector>

#ifdef _WIN32
#include <process.h>
#ifndef WIN32_LEAN_AND_MEAN
#define NEVERD_STACKSIZEMAIN_UNDEF_WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NEVERD_STACKSIZEMAIN_UNDEF_NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef NEVERD_STACKSIZEMAIN_UNDEF_NOMINMAX
#undef NOMINMAX
#undef NEVERD_STACKSIZEMAIN_UNDEF_NOMINMAX
#endif
#ifdef NEVERD_STACKSIZEMAIN_UNDEF_WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#undef NEVERD_STACKSIZEMAIN_UNDEF_WIN32_LEAN_AND_MEAN
#endif
#else
#include <pthread.h>
#endif

namespace neverd {

inline constexpr std::size_t kDecompilerMainStackSize = 128 * 1024 * 1024;

/// Darwin gives ordinary pthreads a 512 KiB stack.  Match LLVM's 8 MiB Darwin
/// worker stack so recursive pipeline phases have headroom without reserving a
/// full decompiler-main stack for every core.
inline constexpr std::size_t kDecompilerWorkerStackSize = 8 * 1024 * 1024;

namespace detail {

struct LargeStackMainArgs {
  int (*Fn)(int, char *[]);
  int Argc;
  char **Argv;
  int Result;
};

template <typename Fn> struct LargeStackWorkerArgs {
  Fn *Worker;
};

#ifdef _WIN32
inline unsigned __stdcall largeStackThreadEntry(void *Arg) {
  auto *A = static_cast<LargeStackMainArgs *>(Arg);
  A->Result = A->Fn(A->Argc, A->Argv);
  return 0;
}

template <typename Fn>
inline unsigned __stdcall largeStackWorkerEntry(void *Arg) {
  auto *A = static_cast<LargeStackWorkerArgs<Fn> *>(Arg);
  (*A->Worker)();
  return 0;
}
#else
inline void *largeStackThreadEntry(void *Arg) {
  auto *A = static_cast<LargeStackMainArgs *>(Arg);
  A->Result = A->Fn(A->Argc, A->Argv);
  return nullptr;
}

template <typename Fn> inline void *largeStackWorkerEntry(void *Arg) {
  auto *A = static_cast<LargeStackWorkerArgs<Fn> *>(Arg);
  (*A->Worker)();
  return nullptr;
}
#endif

} // namespace detail

/// Run \p Count copies of \p Worker concurrently on large-stack threads.
/// If native thread creation stops early, the caller drains the shared work
/// queue after joining the workers that started, preserving correct results
/// with reduced parallelism.
template <typename Fn>
void runWithLargeStackThreads(unsigned Count, Fn &Worker) {
  if (Count == 0)
    return;
  detail::LargeStackWorkerArgs<Fn> Args{&Worker};

#ifdef _WIN32
  std::vector<HANDLE> Threads;
  Threads.reserve(Count);
  bool NeedsInlineWorker = false;
  for (unsigned I = 0; I < Count; ++I) {
    auto Thread = ::_beginthreadex(
        nullptr, static_cast<unsigned>(kDecompilerWorkerStackSize),
        detail::largeStackWorkerEntry<Fn>, &Args,
        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (Thread == 0) {
      NeedsInlineWorker = true;
      break;
    }
    Threads.push_back(reinterpret_cast<HANDLE>(Thread));
  }
  for (HANDLE Thread : Threads) {
    ::WaitForSingleObject(Thread, INFINITE);
    ::CloseHandle(Thread);
  }
  if (NeedsInlineWorker)
    Worker();
#else
  pthread_attr_t Attr;
  if (pthread_attr_init(&Attr) != 0) {
    Worker();
    return;
  }
  if (pthread_attr_setstacksize(&Attr, kDecompilerWorkerStackSize) != 0) {
    pthread_attr_destroy(&Attr);
    Worker();
    return;
  }

  std::vector<pthread_t> Threads;
  Threads.reserve(Count);
  bool NeedsInlineWorker = false;
  for (unsigned I = 0; I < Count; ++I) {
    pthread_t Thread;
    if (pthread_create(&Thread, &Attr, detail::largeStackWorkerEntry<Fn>,
                       &Args) != 0) {
      NeedsInlineWorker = true;
      break;
    }
    Threads.push_back(Thread);
  }
  pthread_attr_destroy(&Attr);
  for (pthread_t Thread : Threads)
    pthread_join(Thread, nullptr);
  if (NeedsInlineWorker)
    Worker();
#endif
}

/// Run \p RealMain on a thread with a 128 MiB stack.  Falls back to a
/// direct call if thread creation fails.
inline int runWithLargeStack(int (*RealMain)(int, char *[]), int Argc,
                             char *Argv[]) {
  detail::LargeStackMainArgs A{RealMain, Argc, Argv, 0};

#ifdef _WIN32
  auto Thread =
      ::_beginthreadex(nullptr, static_cast<unsigned>(kDecompilerMainStackSize),
                       detail::largeStackThreadEntry, &A,
                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (Thread == 0)
    return RealMain(Argc, Argv);
  HANDLE Handle = reinterpret_cast<HANDLE>(Thread);
  ::WaitForSingleObject(Handle, INFINITE);
  ::CloseHandle(Handle);
#else
  pthread_t Tid;
  pthread_attr_t Attr;
  if (pthread_attr_init(&Attr) != 0)
    return RealMain(Argc, Argv);
  if (pthread_attr_setstacksize(&Attr, kDecompilerMainStackSize) != 0) {
    pthread_attr_destroy(&Attr);
    return RealMain(Argc, Argv);
  }

  if (pthread_create(&Tid, &Attr, detail::largeStackThreadEntry, &A) != 0) {
    pthread_attr_destroy(&Attr);
    return RealMain(Argc, Argv);
  }
  pthread_attr_destroy(&Attr);
  pthread_join(Tid, nullptr);
#endif
  return A.Result;
}

} // namespace neverd

#endif // NEVERD_SUPPORT_STACKSIZEMAIN_H
