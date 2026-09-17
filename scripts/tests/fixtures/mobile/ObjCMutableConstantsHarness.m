#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface NDMutableConstants : NSObject
- (id)value;
- (id)independent;
- (id)initial;
- (void)setValue:(id)value;
- (void)setIndependent:(id)value;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDMutableConstants *a = [NDMutableConstants new];
    NDMutableConstants *b = [NDMutableConstants new];
    id initial = [a initial];
    dispatch_apply(64, dispatch_get_global_queue(0, 0), ^(size_t i) {
      (void)i;
      if ([a value] != initial || [b independent] != initial)
        abort();
    });
    if ([a value] != initial || [b value] != initial ||
        [a independent] != initial)
      abort();
    for (unsigned i = 0; i != 1024; ++i) {
      id replacement = [[NSObject alloc] init];
      [a setValue:replacement];
      [replacement release];
      if ([b value] != replacement || [a value] != replacement ||
          [b independent] != initial)
        abort();
      [b setValue:nil];
      if ([a value] != nil || [b value] != nil || [a independent] != initial)
        abort();
    }
    [b setIndependent:nil];
    if ([a independent] != nil || [a initial] != initial)
      abort();
    [a release];
    [b release];
    puts("mutable-initializers=1024\nshared-cells=pass\ninitial-identity="
         "pass\nnull-stores=pass");
  }
}
