//===- pacga_oracle.c - Native generic signing comparisons --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Generic signing remains available in an ordinary arm64 process on Apple
/// Silicon. Compare generated C with an independent instruction oracle, and
/// require an actual nonzero signature rather than accepting a NOP profile.
//===----------------------------------------------------------------------===//

#include <stdint.h>

static uint64_t sign_generic(uint64_t Value, uint64_t Modifier) {
  uint64_t Result;
  __asm__ volatile("pacga %0, %1, %2"
                   : "=r"(Result)
                   : "r"(Value), "r"(Modifier));
  return Result;
}

int main(void) {
  const uint64_t Values[] = {0, 1, 0x1234, UINT64_MAX,
                             UINT64_C(0xfedcba9876543210)};
  const uint64_t Modifiers[] = {0, 1, UINT64_MAX, UINT64_C(0x123456789abcdef0)};
  int NonzeroSignature = 0;
  for (unsigned V = 0; V < sizeof(Values) / sizeof(Values[0]); ++V)
    for (unsigned M = 0; M < sizeof(Modifiers) / sizeof(Modifiers[0]); ++M) {
      const uint64_t Actual = test_pacga(Values[V], Modifiers[M]);
      if (Actual != sign_generic(Values[V], Modifiers[M]) ||
          (Actual & UINT32_MAX) != 0)
        return 1;
      NonzeroSignature |= Actual != 0;
    }
  return NonzeroSignature ? 0 : 2;
}
