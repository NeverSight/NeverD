#import "ObjCScalarConstants.h"

#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static uint64_t doubleBits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint32_t floatBits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  @autoreleasepool {
    NDScalarConstants *driver = [NDScalarConstants new];
    for (unsigned i = 0; i < 1024; ++i) {
      // Use an unaligned destination and guards to expose width/offset errors.
      unsigned char storage[18];
      memset(storage, 0xa5, sizeof(storage));
      [driver fillWide:storage + 1];
      uint64_t words[2];
      memcpy(words, storage + 1, sizeof(words));
      if (words[0] != UINT64_C(0xfedcba9876543210) ||
          words[1] != UINT64_C(0x8123456789abcdef) || storage[0] != 0xa5 ||
          storage[17] != 0xa5)
        return 2;
      if (doubleBits([driver finiteDouble]) != UINT64_C(0x41323456789abcde) ||
          doubleBits([driver negativeZeroDouble]) !=
              UINT64_C(0x8000000000000000) ||
          doubleBits([driver payloadDouble]) != UINT64_C(0x7ff8000001234567) ||
          floatBits([driver finiteFloat]) != UINT32_C(0x4991a2b4) ||
          floatBits([driver negativeZeroFloat]) != UINT32_C(0x80000000) ||
          floatBits([driver payloadFloat]) != UINT32_C(0x7fc12345))
        return 1;
    }
    puts("scalar-bit-checks=7168\nsigned-zero=pass\nnan-payload=pass\nwide-"
         "lanes=pass");
    [driver release];
  }
  return 0;
}
