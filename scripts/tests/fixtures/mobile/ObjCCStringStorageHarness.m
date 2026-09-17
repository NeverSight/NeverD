#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface NDCStringStorage : NSObject
- (const char *)loadedLabel;
- (const char *)loadedSuffix;
- (const char *)label;
- (const char *)suffix;
- (dispatch_queue_t)newQueue;
- (NSString *)string;
- (id)stored;
- (void)setStored:(id)value;
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDCStringStorage *a = [NDCStringStorage new];
    NDCStringStorage *b = [NDCStringStorage new];
    const char *label = [a label];
    dispatch_queue_t queue = [a newQueue];
    [a setStored:b];
    for (unsigned i = 0; i != 4096; ++i) {
      if ([b label] != label || [a suffix] != label + 8 ||
          [a loadedLabel] != label || [b loadedSuffix] != label + 8 ||
          [[a string] UTF8String] != label || [a stored] != b ||
          objc_getAssociatedObject(a, label + 8) != b ||
          strcmp([b suffix], "queue-label") ||
          strcmp(dispatch_queue_get_label(queue), "persist-queue-label"))
        abort();
    }
    [a setStored:nil];
    if (objc_getAssociatedObject(a, label + 8))
      abort();
    dispatch_release(queue);
    [a release];
    [b release];
    puts("cstring-checks=4096\ninterior-aliases=pass\nretained-label=pass");
  }
}
