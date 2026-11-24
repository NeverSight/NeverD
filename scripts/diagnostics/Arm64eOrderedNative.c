//===- Arm64eOrderedNative.c - Observe ordered accesses on Darwin ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run in a standalone Darwin process. For a synchronous abort the OS
/// supplies the interrupted PC, syndrome and fault address to the handler.
//===----------------------------------------------------------------------===//

#include "Arm64eOrderedAccess.h"
#include "Arm64eOrderedModel.def"

#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>

static sigjmp_buf Recovery;
static volatile sig_atomic_t Active, Signal, ProbingSIMD;
static _Atomic uint64_t SavedESR, SavedFAR, SavedWord;
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, ORDERED_NATIVE_ATOMIC_ERROR);

static void catchAccess(int Number, siginfo_t *Info, void *Context) {
  if (!Active)
    _Exit(EXIT_FAILURE);
  const mcontext_t MC = ((ucontext_t *)Context)->uc_mcontext;
  if (Number == SIGBUS) {
    if (Info->si_code != BUS_ADRALN ||
        (uintptr_t)Info->si_addr != MC->__es.__far)
      _Exit(EXIT_FAILURE);
  } else if (Number != SIGILL || !ProbingSIMD) {
    _Exit(EXIT_FAILURE);
  }
  atomic_store_explicit(&SavedESR, MC->__es.__esr, memory_order_relaxed);
  atomic_store_explicit(&SavedFAR, MC->__es.__far, memory_order_relaxed);
  atomic_store_explicit(&SavedWord, *(const uint32_t *)(uintptr_t)MC->__ss.__pc,
                        memory_order_relaxed);
  Signal = Number;
  siglongjmp(Recovery, 1);
}

bool observeOrdered(uint32_t Word, uintptr_t Base, unsigned Lane, bool Original,
                    bool MayFault, uint64_t *Result, OrderedAbort *Abort) {
  (void)MayFault;
  if (!sigsetjmp(Recovery, 1)) {
    Active = 1;
    *Result = accessOrdered(Word, Base, Lane, Original);
    Active = 0;
    return false;
  }
  Active = 0;
  Abort->ESR = atomic_load_explicit(&SavedESR, memory_order_relaxed);
  Abort->FAR = atomic_load_explicit(&SavedFAR, memory_order_relaxed);
  Abort->Word = atomic_load_explicit(&SavedWord, memory_order_relaxed);
  return true;
}

static bool feature(const char *Name) {
  int Value = 0;
  size_t Size = sizeof(Value);
  return !sysctlbyname(Name, &Value, &Size, NULL, 0) && Size == sizeof(Value) &&
         Value != 0;
}

int main(void) {
  if (!feature(ORDERED_NATIVE_LSE2) || !feature(ORDERED_NATIVE_LRCPC2)) {
    puts(ORDERED_NATIVE_UNAVAILABLE);
    return ORDERED_NATIVE_SKIP;
  }
  struct sigaction Action = {0};
  Action.sa_sigaction = catchAccess;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGBUS, &Action, NULL) || sigaction(SIGILL, &Action, NULL))
    return EXIT_FAILURE;

  // Darwin may omit the optional feature sysctl. Require both aligned Q
  // instructions to agree and identify the exact original word on SIGILL.
  _Alignas(ORDERED_QUAD_BYTES) uint64_t Data[ORDERED_LANES] = {0};
  const uint32_t Words[] = {ORDERED_NATIVE_Q_STORE, ORDERED_NATIVE_Q_LOAD};
  bool WithSIMD = false;
  ProbingSIMD = 1;
  for (unsigned I = 0; I != sizeof(Words) / sizeof(*Words); ++I) {
    uint64_t Result;
    OrderedAbort Abort;
    const bool Fault = observeOrdered(Words[I], (uintptr_t)Data, 0, true, true,
                                      &Result, &Abort);
    if (Fault && (Signal != SIGILL || Abort.Word != Words[I]))
      return EXIT_FAILURE;
    if (I == 0)
      WithSIMD = !Fault;
    else if (WithSIMD != !Fault || (WithSIMD && Result != ORDERED_STORE_LOW))
      return EXIT_FAILURE;
  }
  ProbingSIMD = 0;
  if (Data[0] != (WithSIMD ? ORDERED_STORE_LOW : 0) ||
      Data[1] != (WithSIMD ? ORDERED_STORE_HIGH : 0))
    return EXIT_FAILURE;
  puts(WithSIMD ? ORDERED_NATIVE_GPR_SIMD : ORDERED_NATIVE_GPR);

  // EL0 cannot read SCTLR_EL1. First observe two original GPR loads, then
  // validate the resulting rule against every original and generated access.
  _Alignas(ORDERED_QUAD_BYTES) uint8_t Bytes[ORDERED_QUAD_BYTES * 2] = {0};
  const unsigned Residues[] = {1, ORDERED_QUAD_BYTES - 1};
  bool Faults[sizeof(Residues) / sizeof(*Residues)];
  for (unsigned I = 0; I != sizeof(Residues) / sizeof(*Residues); ++I) {
    const uintptr_t Address = (uintptr_t)(Bytes + Residues[I]);
    uint64_t Value;
    OrderedAbort Abort;
    Faults[I] = observeOrdered(ORDERED_NATIVE_X_LOAD, Address, 0, true, true,
                               &Value, &Abort);
    if (Faults[I] &&
        (Abort.FAR != Address || Abort.Word != ORDERED_NATIVE_X_LOAD ||
         (Abort.ESR & ORDERED_DFSC_MASK) != ORDERED_ALIGNMENT_FAULT))
      return EXIT_FAILURE;
  }
  if (Faults[0] && !Faults[1])
    return EXIT_FAILURE;
  const unsigned Mode = Faults[0]   ? ORDERED_MODE_A
                        : Faults[1] ? 0
                                    : ORDERED_MODE_NAA;
  static const char *const Rules[] = {[0] = ORDERED_NATIVE_GRANULE,
                                      [ORDERED_MODE_A] = ORDERED_NATIVE_NATURAL,
                                      [ORDERED_MODE_NAA] =
                                          ORDERED_NATIVE_UNALIGNED};
  const char *Name = Rules[Mode];
  const int Failed = compareOrdered(Mode, ORDERED_EL0, WithSIMD);
  if (Failed) {
    fprintf(stderr, ORDERED_NATIVE_MISMATCH, Name, Failed);
    return EXIT_FAILURE;
  }
  puts(Name);
  return EXIT_SUCCESS;
}
