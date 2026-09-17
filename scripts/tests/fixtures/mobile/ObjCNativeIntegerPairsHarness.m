#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDNativeIntegerPairs : NSObject
- (NSUInteger)first:(NSUInteger)first
             second:(NSUInteger)second
               mode:(NSUInteger)mode
             output:(uintptr_t *)output;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static void expected(uint64_t first, uint64_t second, uint64_t mode,
                     uint64_t out[2]) {
  second = mode & 1 ? second + first : second ^ first;
  for (unsigned i = 0; i < (mode & 7); ++i) {
    first = first * 3 + second;
    second = (second << 7) | (second >> 57);
  }
  out[0] = first ^ UINT64_C(0x8123456789abcdef);
  out[1] = second;
}

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDNativeIntegerPairs *driver = [NDNativeIntegerPairs new];
    uint64_t random = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 8192; ++i) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      const uint64_t first = i == 0 ? 0 : i == 1 ? UINT64_MAX : random;
      const uint64_t second = i == 0 ? UINT64_MAX : ~random;
      const uint64_t mode = i & 15;
      uint64_t a[2], b[2], c[2];
      expected(first, second, mode, a);
      expected(second, first, mode ^ 1, b);
      expected(first ^ 13, second + 7, mode + 1, c);
      uintptr_t output[5] = {random, 0, 0, 0, UINT64_C(0xf0e1d2c3b4a59687)};
      const uint64_t result = [driver first:first
                                     second:second
                                       mode:mode
                                     output:output];
      if (result != (a[0] ^ b[1]) || output[0] != random + 3 ||
          output[1] != a[1] || output[2] != b[0] || output[3] != c[0] ||
          output[4] != UINT64_C(0xf0e1d2c3b4a59687))
        abort();
    }
    [driver release];
    puts("native-pair-cases=8192\nboth-words=pass\ncall-effects=24576");
  }
}
