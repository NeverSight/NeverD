//===- pauth_lr_oracle.c - Native two-modifier pointer oracle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent PAuth_LR instructions qualify both keys and compare the lifted
/// LLVM and C projections. Included after the generated test functions.
//===----------------------------------------------------------------------===//

#include <stdint.h>

static uint64_t oracle_sign_a(uint64_t pointer, uint64_t first,
                              uint64_t second) {
  uint64_t result;
  __asm__ volatile("mov x17, %1\nmov x16, %2\nmov x15, %3\n"
                   ".inst 0xdac18bfe\nmov %0, x17"
                   : "=r"(result)
                   : "r"(pointer), "r"(first), "r"(second)
                   : "x15", "x16", "x17");
  return result;
}

static uint64_t oracle_sign_b(uint64_t pointer, uint64_t first,
                              uint64_t second) {
  uint64_t result;
  __asm__ volatile("mov x17, %1\nmov x16, %2\nmov x15, %3\n"
                   ".inst 0xdac18ffe\nmov %0, x17"
                   : "=r"(result)
                   : "r"(pointer), "r"(first), "r"(second)
                   : "x15", "x16", "x17");
  return result;
}

int main(void) {
  int a_changes = 0, b_changes = 0, a_diversifies = 0, b_diversifies = 0;
  for (uint64_t i = 0; i != 64; ++i) {
    uint64_t pointer = 0x12340 + i * 16;
    uint64_t first = i * 0x12345678;
    uint64_t second = i * 0x98765432;
    uint64_t a = oracle_sign_a(pointer, first, second);
    uint64_t b = oracle_sign_b(pointer, first, second);
    a_changes |= a != pointer;
    b_changes |= b != pointer;
    a_diversifies |= a != oracle_sign_a(pointer, first, second + 4);
    b_diversifies |= b != oracle_sign_b(pointer, first, second + 4);
    if (test_pacia2(pointer, first, second) != a ||
        test_pacib2(pointer, first, second) != b)
      return 1;
    if (test_autia2(a, first, second) != pointer ||
        test_autib2(b, first, second) != pointer)
      return 2;
    if (test_returnia2(a, first, second) != pointer ||
        test_returnib2(b, first, second) != pointer)
      return 3;
  }
  return !(a_changes && b_changes && a_diversifies && b_diversifies);
}
