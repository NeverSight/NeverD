#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern const unsigned char NDIntMetadata[] __asm__("_$sSiN");
extern const unsigned char NDUIntMetadata[] __asm__("_$sSuN");
extern const unsigned char NDStringMetadata[] __asm__("_$sSSN");
extern const unsigned char NDBoolMetadata[] __asm__("_$sSbN");
extern const unsigned char NDDoubleMetadata[] __asm__("_$sSdN");
extern void *swift_projectBox(void *);
extern void swift_release(void *);
@interface NDNativeRecordResults : NSObject
- (void *)newBox:(const void *)metadata
           value:(NSUInteger)value
         storage:(void **)storage;
- (NSUInteger)metadataState:(const void *)metadata
                    request:(NSUInteger)request
                     result:(const void **)result;
- (NSUInteger)unionStart:(NSUInteger)a
                  length:(NSUInteger)b
              otherStart:(NSUInteger)c
                  length:(NSUInteger)d
                  output:(NSUInteger *)output;
- (NSUInteger)intersectionStart:(NSUInteger)a
                         length:(NSUInteger)b
                     otherStart:(NSUInteger)c
                         length:(NSUInteger)d
                         output:(NSUInteger *)output;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDNativeRecordResults *driver = [NDNativeRecordResults new];
    const void *types[] = {NDIntMetadata, NDUIntMetadata, NDStringMetadata,
                           NDBoolMetadata, NDDoubleMetadata};
    uint64_t random = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 8192; ++i) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      const uint64_t value = i == 0   ? 0
                             : i == 1 ? UINT64_MAX
                             : i == 2 ? UINT64_C(1) << 63
                                      : random;
      void *storage = 0;
      void *box = [driver newBox:NDUIntMetadata value:value storage:&storage];
      if (!box || !storage || storage != swift_projectBox(box) ||
          *(uintptr_t *)storage != value)
        abort();
      swift_release(box);
      const void *result = 0;
      const void *metadata = types[i % 5];
      const NSUInteger state = [driver metadataState:metadata
                                             request:0
                                              result:&result];
      if (result != metadata || state != 0)
        abort();
    }
    for (unsigned i = 0; i < 8192; ++i) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      const NSRange a = NSMakeRange(random & 0xffff, (random >> 16) & 0xffff);
      const NSRange b =
          NSMakeRange((random >> 32) & 0xffff, (random >> 48) & 0xffff);
      const NSRange u = NSUnionRange(a, b), x = NSIntersectionRange(a, b);
      NSUInteger output[] = {UINT64_MAX, 17, UINT64_C(0xfedcba9876543210)};
      NSUInteger first = [driver unionStart:a.location
                                     length:a.length
                                 otherStart:b.location
                                     length:b.length
                                     output:output];
      if (first != u.location || output[0] != u.length || output[1] != 18 ||
          output[2] != UINT64_C(0xfedcba9876543210))
        abort();
      NSUInteger second = [driver intersectionStart:a.location
                                             length:a.length
                                         otherStart:b.location
                                             length:b.length
                                             output:output];
      if (second != x.length || output[0] != x.location || output[1] != 19 ||
          output[2] != UINT64_C(0xfedcba9876543210))
        abort();
    }
    [driver release];
    puts("native-record-result-cases=16384\nbox-storage=pass\nmetadata-"
         "response=pass\nnative-record-argument-cases=16384");
  }
}
