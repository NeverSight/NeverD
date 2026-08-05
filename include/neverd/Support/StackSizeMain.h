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

#include <pthread.h>

namespace neverd {

/// Run \p RealMain on a thread with a 128 MiB stack.  Falls back to a
/// direct call if pthread_create fails.
inline int runWithLargeStack(int (*RealMain)(int, char *[]), int Argc,
                             char *Argv[]) {
  constexpr size_t StackSize = 128 * 1024 * 1024;

  struct Args {
    int (*Fn)(int, char *[]);
    int Argc;
    char **Argv;
    int Result;
  };

  auto ThreadEntry = [](void *Arg) -> void * {
    auto *A = static_cast<Args *>(Arg);
    A->Result = A->Fn(A->Argc, A->Argv);
    return nullptr;
  };

  Args A{RealMain, Argc, Argv, 0};

  pthread_t Tid;
  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, StackSize);

  if (pthread_create(&Tid, &Attr, ThreadEntry, &A) != 0) {
    pthread_attr_destroy(&Attr);
    return RealMain(Argc, Argv);
  }
  pthread_attr_destroy(&Attr);
  pthread_join(Tid, nullptr);
  return A.Result;
}

} // namespace neverd

#endif // NEVERD_SUPPORT_STACKSIZEMAIN_H
