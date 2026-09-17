#import <Foundation/Foundation.h>
#include <stdint.h>

typedef struct {
  uintptr_t first, second;
} NDIntegerPair;

static __attribute__((noinline)) NDIntegerPair makeIntegerPair(
    uintptr_t first, uintptr_t second, uintptr_t mode, uintptr_t *effects) {
  ++*effects;
  if (mode & 1)
    second += first;
  else
    second ^= first;
  for (unsigned i = 0; i < (mode & 7); ++i) {
    first = first * 3 + second;
    second = (second << 7) | (second >> 57);
  }
  return (NDIntegerPair){first ^ UINT64_C(0x8123456789abcdef), second};
}

@interface NDNativeIntegerPairs : NSObject
- (NSUInteger)first:(NSUInteger)first
             second:(NSUInteger)second
               mode:(NSUInteger)mode
             output:(uintptr_t *)output;
@end
@implementation NDNativeIntegerPairs
- (NSUInteger)first:(NSUInteger)first
             second:(NSUInteger)second
               mode:(NSUInteger)mode
             output:(uintptr_t *)output {
  NDIntegerPair a = makeIntegerPair(first, second, mode, output);
  NDIntegerPair b = makeIntegerPair(second, first, mode ^ 1, output);
  // A third call observes only its first word; the common inferred callee
  // contract must also serve this scalar use without duplicating effects.
  NDIntegerPair c = makeIntegerPair(first ^ 13, second + 7, mode + 1, output);
  output[1] = a.second;
  output[2] = b.first;
  output[3] = c.first;
  return a.first ^ b.second;
}
@end
