#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDDispatchOnce : NSObject
+ (id)shared;
+ (NSUInteger)initializationCount;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    if ([NDDispatchOnce initializationCount] != 0)
      abort();
    id first = [NDDispatchOnce shared];
    if (!first || ![first isKindOfClass:[NDDispatchOnce class]])
      abort();
    for (unsigned i = 0; i < 8192; ++i)
      if ([NDDispatchOnce shared] != first ||
          [NDDispatchOnce initializationCount] != 1)
        abort();
    puts("dispatch-once-calls=8192\ninitializer-effects=once\nshared-object="
         "pass");
  }
}
