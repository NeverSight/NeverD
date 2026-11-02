//===- pauth_oracle.c - Native pointer-authentication comparisons -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Appended to generated C on a PAuth-capable Apple Silicon host. Independent
/// machine instructions use this process's real keys; no disabled-key model
/// or predictable signature value participates in the comparison.
//===----------------------------------------------------------------------===//

#include <stdint.h>

static uint64_t sign_ia(uint64_t Value, uint64_t Modifier) {
  __asm__ volatile("pacia %0, %1" : "+r"(Value) : "r"(Modifier));
  return Value;
}

static uint64_t sign_ib(uint64_t Value, uint64_t Modifier) {
  __asm__ volatile("pacib %0, %1" : "+r"(Value) : "r"(Modifier));
  return Value;
}

static uint64_t sign_da(uint64_t Value, uint64_t Modifier) {
  __asm__ volatile("pacda %0, %1" : "+r"(Value) : "r"(Modifier));
  return Value;
}

static uint64_t sign_db(uint64_t Value, uint64_t Modifier) {
  __asm__ volatile("pacdb %0, %1" : "+r"(Value) : "r"(Modifier));
  return Value;
}

static uint64_t sign_generic(uint64_t Value, uint64_t Modifier) {
  uint64_t Result;
  __asm__ volatile("pacga %0, %1, %2"
                   : "=r"(Result)
                   : "r"(Value), "r"(Modifier));
  return Result;
}

int main(void) {
  const uint64_t Values[] = {0, 1, 0x1234, 0x7ffffff0};
  const uint64_t Modifiers[] = {0, 1, UINT64_MAX, UINT64_C(0x123456789abcdef0)};
  unsigned ChangedKeys = 0;
  int GenericSignature = 0;
  for (unsigned V = 0; V < sizeof(Values) / sizeof(Values[0]); ++V)
    for (unsigned M = 0; M < sizeof(Modifiers) / sizeof(Modifiers[0]); ++M) {
      const uint64_t P = Values[V], D = Modifiers[M];
      const uint64_t IA = sign_ia(P, D), IB = sign_ib(P, D);
      const uint64_t DA = sign_da(P, D), DB = sign_db(P, D);
      ChangedKeys |=
          (IA != P) | ((IB != P) << 1) | ((DA != P) << 2) | ((DB != P) << 3);
      if (test_pacia(P, D) != IA || test_pacib(P, D) != IB ||
          test_pacda(P, D) != DA || test_pacdb(P, D) != DB)
        return 1;
      if (test_autia(IA, D) != P || test_autib(IB, D) != P ||
          test_autda(DA, D) != P || test_autdb(DB, D) != P)
        return 2;
      if (test_xpaci(IA, D) != P || test_xpaci(IB, D) != P ||
          test_xpacd(DA, D) != P || test_xpacd(DB, D) != P)
        return 3;
      const uint64_t G = test_pacga(P, D);
      if (G != sign_generic(P, D) || (G & UINT32_MAX) != 0)
        return 4;
      GenericSignature |= G != 0;
    }
  // Feature discovery alone does not establish that pointer signing is
  // enabled. Every key must have changed at least one input in this process.
  if (ChangedKeys != 15 || !GenericSignature)
    return 5;
  return 0;
}
