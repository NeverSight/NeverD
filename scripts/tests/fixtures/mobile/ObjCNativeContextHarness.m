#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
@interface NDNativeContext : NSObject
- (uint64_t)word:(uint64_t)value context:(const uint64_t *)context;
- (uint64_t)contextWord:(const uint64_t *)context;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDNativeContext *driver = [NDNativeContext new];
    uint64_t state = UINT64_MAX;
    unsigned cases = 0;
    for (unsigned i = 0; i < 8192; ++i) {
      state =
          state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
      uint64_t value = i == 0 ? UINT64_MAX : i == 1 ? 0 : state;
      uint64_t context =
          i < 64 ? UINT64_C(1) << i : state ^ UINT64_C(0x8000000000000001);
      const uint64_t saved = context;
      if ([driver word:value
               context:&context] != value + context + 19 + (uint32_t)getpid() ||
          [driver contextWord:&context] != context || context != saved)
        abort();
      cases += 2;
    }
    [driver release];
    if (cases != 16384)
      abort();
    puts("native-context-cases=16384\ncontext-bits=pass\nmemory-input=pass");
  }
}
