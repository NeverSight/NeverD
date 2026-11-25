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
extern Quadword storeOrderedQ(uintptr_t, Quadword *, Quadword);
extern Quadword loadOrderedQ(uintptr_t, Quadword *, Quadword);
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

static bool sameQ(Quadword A, Quadword B) {
  return A[0] == B[0] && A[1] == B[1];
}

static int executeQ(bool Store) {
  struct sigaction Action = {0};
  Action.sa_sigaction = illegalInstruction;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGILL, &Action, NULL))
    return EXIT_FAILURE;
  ProbingStore = Store;
  // Hold either source fixed while varying the other. A register-only
  // operation cannot stand in for a load, or a fixed value for a store.
  static const Quadword Inputs[] = ORDERED_REGISTER_INPUTS;
  for (size_t M = 0; M != sizeof(Inputs) / sizeof(*Inputs); ++M) {
    const Quadword Memory = ~Inputs[M];
    for (size_t R = 0; R != sizeof(Inputs) / sizeof(*Inputs); ++R) {
      const Quadword Register = Inputs[R];
      const Quadword Guard = ~Register;
      struct {
        Quadword Before, Value, After;
      } Data = {Guard, Memory, Guard};
      const Quadword Result = Store ? storeOrderedQ(0, &Data.Value, Register)
                                    : loadOrderedQ(0, &Data.Value, Register);
      const Quadword Expected = Store ? Register : Memory;
      if (!sameQ(Result, Expected) || !sameQ(Data.Value, Expected) ||
          !sameQ(Data.Before, Guard) || !sameQ(Data.After, Guard))
        return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
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
