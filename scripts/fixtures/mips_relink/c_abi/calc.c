#include "call.h"

__attribute__((noinline, section(".text.calc")))
struct Result calc(struct Input value, unsigned seed, ...) {
  __builtin_va_list args;
  __builtin_va_start(args, seed);
  unsigned first = __builtin_va_arg(args, unsigned);
  u64 wide = __builtin_va_arg(args, u64);
  int negative = __builtin_va_arg(args, int);
  u64 different = __builtin_va_arg(args, u64);
  int last = __builtin_va_arg(args, int);
  __builtin_va_end(args);

  return (struct Result){value.value ^ wide ^ different,
                         (u64)(value.sign + negative + last) + seed + first,
                         (different ^ value.value) + value.tag + REVISION};
}
