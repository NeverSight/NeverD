#define _GNU_SOURCE 1
#include <stdint.h>
#include <unistd.h>

/* Scalars are read and written through plain pointer casts at any address:
   build for a target with unaligned access, with -fno-strict-aliasing. */


uint64_t arguments(uint64_t next);

uint64_t arguments(uint64_t next) {
    uint64_t text0;
    uint32_t index1;

    text0 = *(uint64_t *)(uintptr_t)(&optarg);
    index1 = *(uint32_t *)(uintptr_t)(&optind);
    *(uint64_t *)(uintptr_t)(&optarg) = next;
    return (text0 + (uint64_t)((uint32_t)(index1)));
}


int main(void) {
  optarg = 0;
  optind = 3;
  if (arguments(7) != 3)
    return 1;
  return optarg == (char *)7 ? 0 : 2;
}
