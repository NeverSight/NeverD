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

#include "pauth_register_oracle.def"

int main(void) { return compareRegisterAndPC(1, 1); }
