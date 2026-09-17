#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static NSUInteger destructions;
@interface NDFrameTracked : NSObject
@end
@implementation NDFrameTracked
- (void)dealloc { ++destructions; [super dealloc]; }
@end
@interface NDVoidFrames : NSObject
- (void)releaseFirst:(void *)first second:(void *)second active:(NSUInteger)active;
- (NSUInteger)releaseFirst:(void *)first second:(void *)second
                   active:(NSUInteger)active result:(NSUInteger)value;
- (void)touchFirst:(void *)first
            second:(void *)second
            active:(NSUInteger)active;
- (NSUInteger)touchFirst:(void *)first
                  second:(void *)second
                  active:(NSUInteger)active
                  result:(NSUInteger)value;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDVoidFrames *driver = [NDVoidFrames new];
    uint64_t random = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 8192; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      NSUInteger active = (i & 1) ? UINT64_MAX : 0;
      NDFrameTracked *first = [NDFrameTracked new];
      NDFrameTracked *second = [NDFrameTracked new];
      NSUInteger before = destructions;
      [driver releaseFirst:first second:second active:active];
      if (destructions != before + (active ? 2 : 0)) abort();
      if (!active) { [first release]; [second release]; }
      active = (i >> 1) & 1;
      first = [NDFrameTracked new]; second = [NDFrameTracked new];
      before = destructions;
      const NSUInteger value = i == 0 ? 0 : i == 1 ? UINT64_MAX : random;
      if ([driver releaseFirst:first second:second active:active result:value] != value ||
          destructions != before + (active ? 2 : 0)) abort();
      if (!active) { [first release]; [second release]; }
      for (unsigned mixed = 0; mixed < 2; ++mixed) {
        active = (i >> mixed) & 1;
        first = [NDFrameTracked new];
        second = [NDFrameTracked new];
        before = destructions;
        if (mixed) {
          if ([driver touchFirst:first
                          second:second
                          active:active
                          result:value] != value)
            abort();
        } else {
          [driver touchFirst:first second:second active:active];
        }
        if (destructions != before + (active ? 1 : 0) ||
            [first retainCount] != 1)
          abort();
        [first release];
        if (!active)
          [second release];
      }
    }
    if (destructions != 65536)
      abort();
    [driver release];
    puts("native-void-frame-cases=32768\nrelease-effects=pass\nindependent-"
         "results=pass\nmixed-call-results=pass");
  }
}
