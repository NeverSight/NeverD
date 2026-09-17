#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
@interface NDBlockFactory : NSObject
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array;
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset;
- (void *)duplicateBlock:(void *)block;
- (void)releaseBlock:(void *)block;
- (NSUInteger)copiedCountForArray:(NSArray *)array offset:(NSUInteger)offset;
- (void)asynchronouslyAppend:(id)value toArray:(NSMutableArray *)array
                       queue:(dispatch_queue_t)queue;
- (void)barrierAppend:(id)value toArray:(NSMutableArray *)array
                queue:(dispatch_queue_t)queue;
- (void)synchronouslyAppend:(id)value
                    toArray:(NSMutableArray *)array
                      queue:(dispatch_queue_t)queue;
- (id (^)(void))holderForBlock:(NSUInteger (^)(void))block;
@end
static unsigned destroyed;
@interface NDLifetimeToken : NSObject
@end
@implementation NDLifetimeToken
- (void)dealloc {
  ++destroyed;
  [super dealloc];
}
@end
#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif
__attribute__((noinline)) static unsigned overwriteStack(unsigned seed) {
  volatile unsigned char bytes[16384];
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    bytes[i] = (i + seed) * 37;
  return bytes[seed % sizeof(bytes)];
}
int main(void) {
#ifdef NEVERD_RECOVERED_ARC
  installRecovered();
#endif
  NDBlockFactory *driver = [NDBlockFactory new];
  if ([driver makeCounterForArray:nil other:nil offset:7] != nil)
    return 8;
  dispatch_queue_t queue = dispatch_queue_create("neverd.source.blocks", NULL);
  for (unsigned i = 0; i < 1024; ++i) {
    NSUInteger (^saved)(void);
    NSUInteger (^combined)(void);
#ifdef NEVERD_MANUAL_BLOCKS
    id (^holder)(void);
#endif
    NSMutableArray *observed;
    @autoreleasepool {
      NDBlockFactory *factory = [NDBlockFactory new];
      NDLifetimeToken *token = [NDLifetimeToken new];
      observed = [[NSMutableArray alloc] initWithObjects:token, nil];
      saved = [[factory makeCounterForArray:observed] copy];
#ifdef NEVERD_MANUAL_BLOCKS
      holder = [[factory holderForBlock:saved] copy];
#endif
      combined = [[factory makeCounterForArray:observed other:observed
                                        offset:i] copy];
      [token release];
      [observed release];
      [factory release];
    }
    (void)overwriteStack(i);
    if (destroyed != i || saved() != 1)
      return 1;
    if ([driver copiedCountForArray:observed offset:i] != i + 1 ||
        destroyed != i)
      return 9;
#ifdef NEVERD_MANUAL_BLOCKS
    if (holder() != (id)saved)
      return 6;
#endif
    NSUInteger (^duplicate)(void) =
        (NSUInteger(^)(void))[driver duplicateBlock:(void *)saved];
    [driver releaseBlock:(void *)saved];
    if (destroyed != i || duplicate() != 1)
      return 2;
    @autoreleasepool {
      [driver synchronouslyAppend:@"retained" toArray:observed queue:queue];
    }
    const NSUInteger expected = ((i & 1) ? 2 : 4) + i;
    if (duplicate() != 2 || combined() != expected)
      return 3;
    if ([driver copiedCountForArray:observed offset:i] != (i ^ 0x55) ||
        destroyed != i)
      return 10;
    [driver releaseBlock:(void *)duplicate];
    if (destroyed != i || combined() != expected)
      return 4;
    [driver releaseBlock:(void *)combined];
#ifdef NEVERD_MANUAL_BLOCKS
    if (destroyed != i || ((NSUInteger(^)(void))holder())() != 2)
      return 7;
    [driver releaseBlock:(void *)holder];
#endif
    if (destroyed != i + 1)
      return 5;
  }
  // Keep both callbacks pending until their construction frames and pools
  // are gone, then execute the runtime-owned copies in submission order.
  NSMutableArray *delayed = [NSMutableArray new];
  dispatch_suspend(queue);
  @autoreleasepool {
    NDLifetimeToken *first = [NDLifetimeToken new];
    NDLifetimeToken *second = [NDLifetimeToken new];
    [driver asynchronouslyAppend:first toArray:delayed queue:queue];
    [driver barrierAppend:second toArray:delayed queue:queue];
    [first release];
    [second release];
  }
  (void)overwriteStack(19);
  if (destroyed != 1024 || delayed.count != 0)
    return 11;
  dispatch_resume(queue);
  dispatch_sync(queue, ^{});
  if (destroyed != 1024 || delayed.count != 2 || delayed[0] == delayed[1])
    return 12;
  [delayed release];
  if (destroyed != 1026)
    return 13;
  [driver release];
  dispatch_release(queue);
  puts("escaping-blocks=1024\ncopy-dispose=pass\nmutated-captures=pass\n"
       "conditional-invokes=1024\nconditional-construction=pass\n"
       "synchronous-mutations=1024\nasync-copied-captures=2");
  return 0;
}
