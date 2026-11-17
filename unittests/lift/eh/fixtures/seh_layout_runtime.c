//===- seh_layout_runtime.c - Run a printed SEH layout against its IR -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Runs FUNC, one function of SEHSharedTryLayoutTests.cpp, for every set of
/// calls that raise and every argument, and writes one trace line per run:
/// `w<n>` for a call of work, `!` where it raises, `f` for the termination
/// handler, `=<n>` for a return and `X` for an exception that leaves FUNC.
/// scripts/check_llvmc_seh_layout_runtime.py builds it twice, beside the IR
/// and beside the C printed from that IR, and compares the traces.  A call
/// raises once for each run that names it, and every call after the twelfth
/// raises, which ends a loop no handler leaves.

#include <stdint.h>
typedef unsigned long DWORD;
__declspec(dllimport) void __stdcall RaiseException(DWORD, DWORD, DWORD,
                                                    const void *);
__declspec(dllimport) void *__stdcall GetStdHandle(DWORD);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, DWORD,
                                              DWORD *, void *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
static char trace[1 << 20];
static unsigned used;
static void put(char c) {
  if (used < sizeof trace)
    trace[used++] = c;
}
static unsigned mask, calls;
void work(uint32_t id) {
  put('w');
  put((char)('0' + id));
  if (++calls > 12 || (mask >> id & 1)) {
    mask &= ~(1u << id);
    put('!');
    RaiseException(0xE0000001, 0, 0, 0);
  }
}
void ended(uint8_t abnormal, void *frame) {
  (void)abnormal;
  (void)frame;
  put('f');
}
#if ARGS == 0
uint32_t FUNC(void);
#define CALL(a, b) FUNC()
#elif ARGS == 1
uint32_t FUNC(uint8_t);
#define CALL(a, b) FUNC(a)
#else
uint32_t FUNC(uint8_t, uint8_t);
#define CALL(a, b) FUNC(a, b)
#endif
static void run(unsigned m, unsigned a, unsigned b) {
  mask = m;
  calls = 0;
  __try {
    uint32_t r = CALL((uint8_t)a, (uint8_t)b);
    put('=');
    put((char)('0' + r));
  } __except (1) {
    put('X');
  }
  put('\n');
}
void entry(void) {
  static const unsigned ids[] = {1, 2, 3, 4, 9};
  for (unsigned s = 0; s < 32; ++s) {
    unsigned m = 0;
    for (unsigned i = 0; i < 5; ++i)
      if (s >> i & 1)
        m |= 1u << ids[i];
    for (unsigned a = 0; a < 2; ++a)
      for (unsigned b = 0; b < 2; ++b)
        run(m, a, b);
  }
  DWORD w;
  WriteFile(GetStdHandle((DWORD)-11), trace, used, &w, 0);
  ExitProcess(0);
}
