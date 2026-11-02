//===- guest.c - Original/translated 3DNow context differential ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// QEMU processor-model state and fault comparisons, not physical CPU proof.
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned U32;
typedef unsigned long long U64;
typedef void (*Operation)(const U64 *);
struct Case {
  Operation compiled;
  Operation original;
  U32 form;
};
#include "cases.inc"

_Alignas(16) U8 captured[512];
U64 resume_stack, fault_error;
U32 fault_vector;
extern U32 run_probe(Operation, const U64 *, const U8 *);
extern void pending_wait(const U64 *);
extern void fault_6(void), fault_12(void), fault_13(void), fault_14(void),
    fault_16(void);

void *memcpy(void *out, const void *in, __SIZE_TYPE__ count) {
  for (__SIZE_TYPE__ i = 0; i < count; ++i)
    ((U8 *)out)[i] = ((const U8 *)in)[i];
  return out;
}
void *memset(void *out, int value, __SIZE_TYPE__ count) {
  for (__SIZE_TYPE__ i = 0; i < count; ++i)
    ((U8 *)out)[i] = value;
  return out;
}
static void put(U8 value) {
  __asm__ volatile("outb %0,%1" ::"a"(value), "d"((U16)0xe9));
}
static void hex(U32 value) {
  for (int shift = 28; shift >= 0; shift -= 4)
    put("0123456789abcdef"[(value >> shift) & 15]);
}
__attribute__((noreturn)) static void finish(U32 value) {
  __asm__ volatile("outl %0,%1" ::"a"(value), "d"((U16)0xf4));
  for (;;)
    __asm__ volatile("hlt");
}
__attribute__((noreturn)) static void fail(U32 index, U32 reason, U32 expected,
                                           U32 actual) {
  put('F');
  hex(index);
  put(':');
  hex(reason);
  put(':');
  hex(expected);
  put(':');
  hex(actual);
  put('\n');
  finish(1);
}

struct __attribute__((packed)) Gate {
  U16 low, selector;
  U8 ist, flags;
  U16 middle;
  U32 high, zero;
};
static struct Gate idt[256];
static void handler(unsigned index, void (*function)(void)) {
  U64 address = (U64)function;
  idt[index] =
      (struct Gate){address, 0x18, 0, 0x8e, address >> 16, address >> 32, 0};
}
static void install_handlers(void) {
  handler(6, fault_6);
  handler(12, fault_12);
  handler(13, fault_13);
  handler(14, fault_14);
  handler(16, fault_16);
  struct __attribute__((packed)) {
    U16 limit;
    U64 base;
  } pointer = {sizeof(idt) - 1, (U64)idt};
  __asm__ volatile("lidt %0" ::"m"(pointer));
}
static const U64 inputs[][2] = {{0x400000003f800000ULL, 0x4080000040400000ULL},
                                {0xc00000003fc00000ULL, 0x3f000000c0800000ULL},
                                {0x0000000100800000ULL, 0x0080000000000001ULL},
                                {0x7fffffff80000000ULL, 0xffffffff00000001ULL}};
static void seed(U8 *state, U32 top, U32 tags, U32 rc, U32 sample) {
  memset(state, 0, 512);
  state[0] = 0x7f;
  state[1] = 3 | (rc << 2);
  state[2] = 0x25;
  state[3] = 0x47 | (top << 3);
  state[4] = tags;
  state[6] = 0x23;
  state[7] = 1;
  *(U64 *)(state + 8) = 0x123400;
  *(U64 *)(state + 16) = 0x556600;
  state[24] = 0x80;
  state[25] = 0x1f;
  for (U32 logical = 0; logical < 8; ++logical) {
    U32 physical = (logical + top) & 7;
    *(U64 *)(state + 32 + 16 * logical) = inputs[sample][physical & 1];
    state[40 + 16 * logical] = 0xff;
    state[41 + 16 * logical] = 0xff;
  }
}
static void compare(U32 index, U32 reason, const U8 *expected) {
  for (U32 byte = 0; byte < 160; ++byte) {
    if (byte == 5 || (byte >= 24 && byte < 32) ||
        (byte >= 32 && (byte - 32) % 16 >= 10))
      continue;
    if (expected[byte] != captured[byte])
      fail(index, reason * 256 + byte, expected[byte], captured[byte]);
  }
}
void guest_main(void) {
  install_handlers();
  U32 a, b, c, d;
  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x80000001), "c"(0));
  const U32 available = (d & 0xc0000000U) == 0xc0000000U;
  U32 normal = 0, faults = 0, missing_mf = 0, stack_gp = 0;
  _Alignas(16) U8 state[512], expected[512];
  seed(state, 5, 0x55, 2, 0);
  state[0] &= ~1U;
  state[2] |= 0x81;
  U32 wait_vector = run_probe(pending_wait, (const U64 *)0, state);
  if (wait_vector != 16)
    fail(0, 0x100, 16, wait_vector);
  for (U32 i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    U64 memory[3] = {0x123456789abcdef0ULL, 0, 0xfedcba9876543210ULL};
    if (available)
      for (U32 top = 0; top < 8; ++top)
        for (U32 tag = 0; tag < 3; ++tag)
          for (U32 rc = 0; rc < 4; ++rc)
            for (U32 sample = 0; sample < 4; ++sample) {
              seed(state, top,
                   tag == 0   ? 0
                   : tag == 1 ? 0x55
                              : 0xff,
                   rc, sample);
              memory[1] = inputs[sample][1];
              U32 original = run_probe(cases[i].original, memory + 1, state);
              if (original)
                fail(i, 1, 0, original);
              memcpy(expected, captured, 512);
              U32 actual = run_probe(cases[i].compiled, memory + 1, state);
              if (actual)
                fail(i, 2, 0, actual);
              compare(i, 3, expected);
              if (memory[0] != 0x123456789abcdef0ULL ||
                  memory[1] != inputs[sample][1] ||
                  memory[2] != 0xfedcba9876543210ULL)
                fail(i, 4, 0, 1);
              ++normal;
            }
    for (U32 mode = available ? 1 : 0; mode < (cases[i].form >= 2 ? 5u : 2u);
         ++mode) {
      seed(state, 5, 0x55, 2, 0);
      if (mode == 1 || mode == 2) {
        state[0] &= ~1U;
        state[2] |= 0x81;
      }
      const U64 *address = mode == 2 || mode == 3 ? (const U64 *)0x40000000ULL
                           : mode == 4 ? (const U64 *)0x800000000000ULL
                                       : memory + 1;
      U32 vector = !available           ? 6
                   : mode <= 2          ? 16
                   : mode == 3          ? 14
                   : cases[i].form == 3 ? 12
                                        : 13;
      U32 original = run_probe(cases[i].original, address, state);
      const U32 model_gap = available && (mode == 1 || mode == 2) &&
                            (original == 0 || (mode == 2 && original == 14));
      const U32 stack_gap =
          available && mode == 4 && cases[i].form == 3 && original == 13;
      if (original != vector && !(ALLOW_QEMU_MISSING_MF && model_gap) &&
          !(ALLOW_QEMU_STACK_GP && stack_gap))
        fail(i, 10 + mode, vector, original);
      U64 error = fault_error;
      memcpy(expected, captured, 512);
      U32 actual = run_probe(cases[i].compiled, address, state);
      if (actual != original)
        fail(i, 20 + mode, original, actual);
      if (error != fault_error)
        fail(i, 30 + mode, error, fault_error);
      compare(i, 40 + mode, expected);
      if (model_gap)
        ++missing_mf;
      else if (stack_gap)
        ++stack_gp;
      else
        ++faults;
    }
  }
  put('P');
  put('A');
  put('S');
  put('S');
  put(':');
  hex(normal);
  put(':');
  hex(faults);
  put(':');
  hex(missing_mf);
  put(':');
  hex(stack_gp);
  put('\n');
  finish(0);
}
