#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDSwiftOnce : NSObject
- (uintptr_t)value;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDSwiftOnce *first = [NDSwiftOnce new];
    NDSwiftOnce *second = [NDSwiftOnce new];
    for (unsigned i = 0; i < 4096; ++i)
      if ([first value] != 101 || [second value] != 101)
        abort();
    [first release];
    [second release];
    puts("swift-once-calls=8192\ninitializer-effects=once\nshared-state=pass");
  }
}
