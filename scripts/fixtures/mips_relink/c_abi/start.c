#include "call.h"

#ifdef CHECK_SIZE_LOOP
const unsigned weights[8] = {3, 5, 7, 11, 13, 17, 19, 23};
#endif

__attribute__((noreturn)) void finish(int value);

#ifdef CHECK_LARGE_FRAME
__attribute__((section(".text.start")))
void fill_frame(unsigned char *frame, unsigned size, unsigned char value) {
  frame[0] = value;
  frame[size - 1] = value;
}
#endif

__attribute__((noreturn, section(".text.start"))) void _start(void) {
  const u64 value = 0x1122334455667788ULL;
  const u64 wide = 0x987654321af9dca5ULL;
  const u64 different = 0x102a304050e06030ULL;
  struct Input input = {value, -47, 0xc7};
  struct Result result = calc(input, 6u, 0x812391abu, wide, -33, different, -7);

  if (result.first != (value ^ wide ^ different) || result.second != 0x8123915aULL)
    finish(121);
  const u64 base = (different ^ value) + 0xc7;
  if (result.third == base + 7)
    finish(43);
  if (result.third == base + 5)
    finish(89);
  finish(122);
}
