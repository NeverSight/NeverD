//===- Arm64eOrderedQ.c - Probe Darwin 128-bit ordered instructions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute the aligned store and load in separate children. A SIGILL qualifies
/// as unavailable only at the instruction being tested.
//===----------------------------------------------------------------------===//

#include "Arm64eOrderedAccess.h"
#include "Arm64eOrderedModel.def"

#if defined(__APPLE__) && defined(__aarch64__)
#include <errno.h>
#include <mach/arm/thread_status.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint64_t Quadword __attribute__((vector_size(ORDERED_QUAD_BYTES)));

// The fixture encodings use X1 for the base and V0 for the transferred lanes.
// A data symbol keeps the instruction address unsigned on the arm64e ABI.
extern Quadword storeOrderedQ(uintptr_t, uint64_t *, Quadword);
extern Quadword loadOrderedQ(uintptr_t, uint64_t *, Quadword);
extern const uint32_t storeOrderedQSite, loadOrderedQSite;
__asm__(ORDERED_NATIVE_Q_ROUTINE(storeOrderedQ, ORDERED_NATIVE_Q_STORE));
__asm__(ORDERED_NATIVE_Q_ROUTINE(loadOrderedQ, ORDERED_NATIVE_Q_LOAD));

static volatile sig_atomic_t ProbingStore;

static void illegalInstruction(int Number, siginfo_t *Info, void *Context) {
  (void)Info;
  const mcontext_t MC = ((ucontext_t *)Context)->uc_mcontext;
  const uintptr_t PC = arm_thread_state64_get_pc(MC->__ss);
  const uint32_t Expected =
      ProbingStore ? ORDERED_NATIVE_Q_STORE : ORDERED_NATIVE_Q_LOAD;
  const uint32_t *Site = ProbingStore ? &storeOrderedQSite : &loadOrderedQSite;
  _exit(Number == SIGILL && PC == (uintptr_t)Site && *Site == Expected
            ? ORDERED_NATIVE_SKIP
            : EXIT_FAILURE);
}

static int executeQ(bool Store) {
  struct sigaction Action = {0};
  Action.sa_sigaction = illegalInstruction;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGILL, &Action, NULL))
    return EXIT_FAILURE;
  ProbingStore = Store;
  // Complementing the input to V0 makes an unchanged or repeated lane fail.
  _Alignas(ORDERED_QUAD_BYTES)
      uint64_t Data[] = {~ORDERED_STORE_LOW, ~ORDERED_STORE_HIGH};
  Quadword V0 = {ORDERED_STORE_LOW, ORDERED_STORE_HIGH};
  if (Store) {
    storeOrderedQ(0, Data, V0);
    return Data[0] == ORDERED_STORE_LOW && Data[1] == ORDERED_STORE_HIGH
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }
  V0 = loadOrderedQ(0, Data, V0);
  return V0[0] == ~ORDERED_STORE_LOW && V0[1] == ~ORDERED_STORE_HIGH &&
                 Data[0] == V0[0] && Data[1] == V0[1]
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}

static int runQ(bool Store) {
  pid_t Child = fork();
  if (Child < 0)
    return EXIT_FAILURE;
  if (!Child)
    _exit(executeQ(Store));
  int Status;
  pid_t Result;
  do {
    Result = waitpid(Child, &Status, 0);
  } while (Result < 0 && errno == EINTR);
  return Result == Child && WIFEXITED(Status) ? WEXITSTATUS(Status)
                                              : EXIT_FAILURE;
}
#endif

OrderedQStatus probeDarwinOrderedQ(void) {
#if defined(__APPLE__) && defined(__aarch64__)
  const int Store = runQ(true), Load = runQ(false);
  if (Store == EXIT_SUCCESS && Load == EXIT_SUCCESS)
    return OrderedQPassed;
  if (Store != ORDERED_NATIVE_SKIP || Load != ORDERED_NATIVE_SKIP)
    return OrderedQFailed;
#endif
  return OrderedQUnavailable;
}

#ifdef ORDERED_NATIVE_Q_PROBE
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  OrderedQStatus Status = probeDarwinOrderedQ();
  if (Status == OrderedQFailed)
    return EXIT_FAILURE;
  puts(Status == OrderedQPassed ? ORDERED_NATIVE_Q_PASSED
                                : ORDERED_NATIVE_Q_UNAVAILABLE);
  return Status == OrderedQPassed ? EXIT_SUCCESS : ORDERED_NATIVE_SKIP;
}
#endif
