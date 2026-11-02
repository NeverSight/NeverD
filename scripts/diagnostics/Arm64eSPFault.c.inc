//===- Arm64eSPFault.c.inc - Native SP exception oracle --------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Appended to an exported harness to observe the actual fault SP, exception
/// class and all neighboring bytes on an alternate signal stack.
//===----------------------------------------------------------------------===//

#include "Arm64eSPFault.def"

#include <mach/arm/thread_status.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ucontext.h>
#include <unistd.h>

_Static_assert(sizeof(SP_TEST_CASES) / sizeof(SP_TEST_CASES[0]) ==
                   SP_TEST_CASE_COUNT,
               SP_TEST_CASE_COUNT_ERROR);

static sigjmp_buf Escape;
static volatile sig_atomic_t Armed;
static volatile uintptr_t ObservedSP;
static volatile uint64_t ObservedESR;

static void captureSPFault(int Signal, siginfo_t *Info, void *Opaque) {
  (void)Signal;
  (void)Info;
  if (!Armed)
    _exit(SP_TEST_UNARMED);
  const ucontext_t *Context = (const ucontext_t *)Opaque;
  ObservedSP = arm_thread_state64_get_sp(Context->uc_mcontext->__ss);
  ObservedESR = Context->uc_mcontext->__es.__esr;
  Armed = 0;
  siglongjmp(Escape, 1);
}

int main(void) {
#if defined(SP_TEST_EXTENDED_MEMORY)
  (void)SP_TEST_EXISTING_CASES;
#endif
  stack_t Alternate = {0};
  Alternate.ss_size = SIGSTKSZ;
  Alternate.ss_sp = malloc(Alternate.ss_size);
  if (!Alternate.ss_sp || sigaltstack(&Alternate, NULL))
    return SP_TEST_ALTSTACK_ERROR;
  struct sigaction Action = {0};
  Action.sa_sigaction = captureSPFault;
  Action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGBUS, &Action, NULL) || sigaction(SIGSEGV, &Action, NULL) ||
      sigaction(SIGILL, &Action, NULL))
    return SP_TEST_HANDLER_ERROR;
  _Alignas(SP_TEST_ALIGNMENT) static unsigned char Guard[SP_TEST_MEMORY_BYTES];
  unsigned Faults = 0;
  for (volatile unsigned C = 0; C != SP_TEST_CASE_COUNT; ++C) {
    for (unsigned I = 0; I != sizeof(Guard); ++I)
      Guard[I] = (unsigned char)(I * SP_TEST_SEED_MULTIPLIER +
                                 (I >> SP_TEST_SEED_HIGH_SHIFT) *
                                     SP_TEST_SEED_HIGH_MULTIPLIER +
                                 SP_TEST_SEED_ADDEND);
    const uintptr_t RawSP = (uintptr_t)&Guard[SP_TEST_BASE_OFFSET];
    ObservedSP = 0;
    ObservedESR = 0;
    if (sigsetjmp(Escape, 1) == 0) {
      uint64_t Upper = 0;
      Armed = 1;
      SP_TEST_CASES[C].call(RawSP, SP_TEST_LOW, SP_TEST_HIGH, &Upper);
      Armed = 0;
      return SP_TEST_MISSING_TRAP;
    }
    if (ObservedSP != RawSP)
      return SP_TEST_WRONG_SP;
    if (ObservedESR >> SP_TEST_ESR_SHIFT != SP_TEST_SP_ALIGNMENT_EC)
      return SP_TEST_WRONG_ESR;
    for (unsigned I = 0; I != sizeof(Guard); ++I) {
      const unsigned char Expected =
          (unsigned char)(I * SP_TEST_SEED_MULTIPLIER +
                          (I >> SP_TEST_SEED_HIGH_SHIFT) *
                              SP_TEST_SEED_HIGH_MULTIPLIER +
                          SP_TEST_SEED_ADDEND);
      if (Guard[I] != Expected)
        return SP_TEST_CHANGED_MEMORY;
    }
    ++Faults;
  }
  printf(SP_TEST_REPORT, Faults);
  return 0;
}
