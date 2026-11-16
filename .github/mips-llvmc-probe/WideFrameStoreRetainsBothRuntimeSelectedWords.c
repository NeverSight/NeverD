#define _GNU_SOURCE 1
#include <stdint.h>

/* Scalars are read and written through plain pointer casts at any address:
   build for a target with unaligned access, with -fno-strict-aliasing. */


uint64_t select_frame_add(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t select_frame_or(uint64_t arg0, uint64_t arg1, uint64_t arg2);

uint64_t select_frame_add(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t v0;
    _Alignas(16) struct {
        uint8_t gap0[48];
        unsigned __int128 var_m18;
        uint8_t gap40[32];
    } frame0;

    frame0.var_m18 = ((__uint128_t)((uint64_t)(arg0)) | ((__uint128_t)((__uint128_t)((uint64_t)(arg1)) << ((__uint128_t)((((__uint128_t)0x0ULL << 64) | (__uint128_t)0x40ULL)) % 128))));
    v0 = *(uint64_t *)((void*)((uint64_t)(uintptr_t)(&frame0.var_m18) + ((uint64_t)((uint64_t)((arg2 & 1)) << 3))));
    return v0;
}

uint64_t select_frame_or(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t v0;
    _Alignas(16) struct {
        uint8_t gap0[48];
        unsigned __int128 var_m18;
        uint8_t gap40[32];
    } frame0;

    frame0.var_m18 = ((__uint128_t)((uint64_t)(arg0)) | ((__uint128_t)((__uint128_t)((uint64_t)(arg1)) << ((__uint128_t)((((__uint128_t)0x0ULL << 64) | (__uint128_t)0x40ULL)) % 128))));
    v0 = *(uint64_t *)((void*)((uint64_t)(uintptr_t)(&frame0.var_m18) | ((uint64_t)((uint64_t)((arg2 & 1)) << 3))));
    return v0;
}


int main(void) {
  const uint64_t values[] = {0, 1, UINT64_MAX, UINT64_C(0x123456789abcdef0),
                            UINT64_C(0xfedcba9876543210)};
  const uint64_t indices[] = {0, 1, 2, 3, 255, 256, UINT64_MAX};
  for (unsigned a = 0; a < sizeof(values) / sizeof(values[0]); ++a)
    for (unsigned b = 0; b < sizeof(values) / sizeof(values[0]); ++b)
      for (unsigned i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
        uint64_t expected = indices[i] & 1 ? values[b] : values[a];
        if (select_frame_add(values[a], values[b], indices[i]) != expected)
          return 1;
        if (select_frame_or(values[a], values[b], indices[i]) != expected)
          return 2;
      }
  return 0;
}
