//===- StackSizeMain.h - Large-stack main() trampoline ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Provides a main() wrapper that spawns the real entry point on a thread
/// with a larger stack (128 MiB) to accommodate deep recursion in the
/// decompiler pipeline.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_STACKSIZEMAIN_H
#define NEVERD_SUPPORT_STACKSIZEMAIN_H

#include <cstddef>

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

namespace detail {

struct LargeStackMainArgs {
  int (*Fn)(int, char *[]);
  int Argc;
  char **Argv;
  int Result;
};

#ifdef _WIN32
inline unsigned __stdcall largeStackThreadEntry(void *Arg) {
  auto *A = static_cast<LargeStackMainArgs *>(Arg);
  A->Result = A->Fn(A->Argc, A->Argv);
  return 0;
}
#else
inline void *largeStackThreadEntry(void *Arg) {
  auto *A = static_cast<LargeStackMainArgs *>(Arg);
  A->Result = A->Fn(A->Argc, A->Argv);
  return nullptr;
}
#endif

} // namespace detail

/// Run \p RealMain on a thread with a 128 MiB stack.  Falls back to a
/// direct call if thread creation fails.
inline int runWithLargeStack(int (*RealMain)(int, char *[]), int Argc,
                             char *Argv[]) {
  constexpr std::size_t StackSize = 128 * 1024 * 1024;
  detail::LargeStackMainArgs A{RealMain, Argc, Argv, 0};

#ifdef _WIN32
  auto Thread = ::_beginthreadex(nullptr, static_cast<unsigned>(StackSize),
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
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, StackSize);

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
