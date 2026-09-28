// Independent register, flag, arithmetic and memory oracle. Code addresses
// come from linked symbols (native) or the loader's symbols (source recovery).
#include <stdint.h>
#include <stdio.h>

#ifndef GENERIC_MACHINE_SOURCE
extern void generic_machine_indirect_call(void);
extern void generic_machine_return_thread(void);
extern void generic_machine_call_zero(void);
extern void generic_machine_call_one(void);
extern void generic_machine_call_fallthrough(void);
extern void generic_machine_return_zero(void);
extern void generic_machine_return_one(void);
extern void generic_machine_observer_exit(void);
extern void generic_machine_observe(uint64_t *, void (*)(void));
#define MACHINE_ADDRESS(symbol) ((uint64_t)(uintptr_t)&symbol)
#endif

static int check_machine(unsigned kind, uint64_t x, uint64_t y,
                         uint64_t flags) {
  uint64_t state[17], expected[17], stack[16], memory[16];
  for (unsigned i = 0; i < 16; ++i) {
    state[i] = UINT64_C(0x317a9dcb4062e850) + i;
    stack[i] = UINT64_C(0x8eca2047d159b630) ^ i;
  }
  state[4] = (uint64_t)(uintptr_t)&stack[8];
  state[6] = y;
  state[7] = x;
  state[16] = flags;
  stack[8] = MACHINE_ADDRESS(generic_machine_observer_exit);
  for (unsigned i = 0; i < 17; ++i)
    expected[i] = state[i];
  for (unsigned i = 0; i < 16; ++i)
    memory[i] = stack[i];
  const uint64_t zero = kind == 0
                            ? MACHINE_ADDRESS(generic_machine_call_zero)
                            : MACHINE_ADDRESS(generic_machine_return_zero);
  const uint64_t one = kind == 0 ? MACHINE_ADDRESS(generic_machine_call_one)
                                 : MACHINE_ADDRESS(generic_machine_return_one);
  const uint64_t selected = (x & 1) ? one : zero;
  expected[0] = x + ((x & 1) ? 4 : 2) * y +
                (kind == 0 ? ((x & 1) ? 29 : 17) : ((x & 1) ? 47 : 31));
  expected[2] = one;
  expected[3] = selected;
  expected[8] = flags;
  expected[11] = state[4];
  if (kind == 0) {
    const uint64_t continuation =
        MACHINE_ADDRESS(generic_machine_call_fallthrough);
    expected[9] = continuation;
    expected[10] = state[4] - 8;
    memory[7] = continuation;
  } else {
    memory[7] = selected;
  }
#ifdef GENERIC_MACHINE_SOURCE
  if (GENERIC_MACHINE_FUNCTION((void *)state) != 0)
    return 1;
#else
  generic_machine_observe(state, kind == 0 ? generic_machine_indirect_call
                                           : generic_machine_return_thread);
  // Source stops at the outer RET; the observer resumes just after it.
  state[4] -= 8;
#endif
  for (unsigned i = 0; i < 17; ++i)
    if (state[i] != expected[i]) {
      fprintf(stderr, "kind=%u register=%u actual=%llx expected=%llx\n", kind,
              i, (unsigned long long)state[i], (unsigned long long)expected[i]);
      return 2;
    }
  for (unsigned i = 0; i < 16; ++i)
    if (stack[i] != memory[i]) {
      fprintf(stderr, "kind=%u stack_slot=%u actual=%llx expected=%llx\n", kind,
              i, (unsigned long long)stack[i], (unsigned long long)memory[i]);
      return 3;
    }
  return 0;
}

int main(void) {
  const unsigned flag_bits[] = {0, 2, 4, 6, 7, 10, 11};
  const uint64_t inputs[] = {
      0,         1, 2, 3, 0x7fffffff, 0x80000000, UINT64_C(0x8000000000000000),
      UINT64_MAX};
  for (unsigned combination = 0; combination < 128; ++combination) {
    uint64_t flags = 0x202;
    for (unsigned i = 0; i < 7; ++i)
      flags |= ((uint64_t)((combination >> i) & 1)) << flag_bits[i];
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
#ifdef GENERIC_MACHINE_KIND
      const unsigned first = GENERIC_MACHINE_KIND;
      const unsigned last = first + 1;
#else
      const unsigned first = 0, last = 2;
#endif
      for (unsigned kind = first; kind < last; ++kind)
        if (check_machine(kind, inputs[i], inputs[7 - i], flags))
          return 1;
    }
  }
  return 0;
}
