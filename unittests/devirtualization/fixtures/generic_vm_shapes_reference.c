// Independent unsigned arithmetic oracle for the original public shape suite.
// No handler addresses, instruction encodings, decoder keys, or virtual stack
// operations occur here. Guards also check the exact observable write range.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#ifndef GENERIC_SHAPE_SOURCE
#if defined(GENERIC_VM_MS_ABI)
#define SHAPE_ABI __attribute__((ms_abi))
#else
#define SHAPE_ABI
#endif
uint64_t SHAPE_ABI generic_vm_pointer_program(uint64_t, uint64_t, uint64_t *);
uint64_t SHAPE_ABI generic_vm_virtual_calls(uint64_t, uint64_t, uint64_t *);
uint64_t SHAPE_ABI generic_vm_rolling_loop(uint64_t, uint64_t, uint64_t *);
#endif

static uint64_t shape_expected(unsigned kind, uint64_t x, uint64_t y,
                               uint64_t *status) {
  if (kind == 0) {
    *status = x & 1;
    return (x + y + (*status ? 43 : 19)) ^ UINT64_C(0x2468ace);
  }
  if (kind == 1) {
    *status = 4;
    const uint64_t first = 3 * (x + y + 9) + 11;
    return 3 * ((first ^ 7) + 9) + 11;
  }
  uint64_t value = x;
  *status = y & 7;
  for (uint64_t i = 0; i < *status; ++i)
    value = (value + y) ^ i;
  return value;
}

static int shape_check(uint64_t x, uint64_t y) {
#ifdef GENERIC_SHAPE_FUNCTION
  for (unsigned kind = GENERIC_SHAPE_KIND; kind < GENERIC_SHAPE_KIND + 1;
       ++kind) {
#else
  for (unsigned kind = 0; kind < 3; ++kind) {
#endif
    uint64_t status;
    const uint64_t expected = shape_expected(kind, x, y, &status);
    const uint64_t before = UINT64_C(0x486159a7bdce023f);
    const uint64_t after = UINT64_C(0x9cb026e847ad153f);
    uint64_t out[] = {before, ~expected, ~status, after};
    uint64_t actual;
#ifdef GENERIC_SHAPE_FUNCTION
    actual = (uint64_t)GENERIC_SHAPE_FUNCTION(x, y, (uintptr_t)&out[1]);
#else
    if (kind == 0)
      actual = generic_vm_pointer_program(x, y, &out[1]);
    else if (kind == 1)
      actual = generic_vm_virtual_calls(x, y, &out[1]);
    else
      actual = generic_vm_rolling_loop(x, y, &out[1]);
#endif
    if (actual != expected || out[1] != expected || out[2] != status ||
        out[0] != before || out[3] != after) {
      fprintf(stderr,
              "shape=%u x=%" PRIx64 " y=%" PRIx64 " actual=%" PRIx64
              " expected=%" PRIx64 " status=%" PRIu64
              " expected_status=%" PRIu64 "\n",
              kind, x, y, actual, expected, out[2], status);
      return 1;
    }
  }
  return 0;
}

int main(void) {
  const uint64_t edges[] = {0,
                            1,
                            2,
                            3,
                            7,
                            8,
                            15,
                            16,
                            31,
                            32,
                            UINT64_C(0xffffffff),
                            UINT64_C(0x100000000),
                            UINT64_C(0x7fffffffffffffff),
                            UINT64_C(0x8000000000000000),
                            UINT64_MAX - 1,
                            UINT64_MAX};
  for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j < sizeof(edges) / sizeof(edges[0]); ++j)
      if (shape_check(edges[i], edges[j]))
        return 1;
  for (uint64_t x = 0; x < 32; ++x)
    for (uint64_t y = 0; y < 32; ++y)
      if (shape_check(x, y))
        return 1;
  uint64_t random = UINT64_C(0x6cab4031975d8ef2);
  for (unsigned i = 0; i < 1024; ++i) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    const uint64_t x = random;
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    if (shape_check(x, random))
      return 1;
  }
  return 0;
}
