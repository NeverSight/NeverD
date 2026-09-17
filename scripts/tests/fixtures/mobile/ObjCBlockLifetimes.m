#include <Block.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
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
#if !__has_feature(objc_arc)
- (id (^)(void))holderForBlock:(NSUInteger (^)(void))block;
#endif
@end
@implementation NDBlockFactory
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array {
  NSUInteger (^block)(void) = ^{
    return array.count;
  };
#if __has_feature(objc_arc)
  return block;
#else
  return [(id)Block_copy(block) autorelease];
#endif
}
- (NSUInteger (^)(void))makeCounterForArray:(NSArray *)array
                                      other:(NSArray *)other
                                     offset:(NSUInteger)offset {
  if (!array)
    return nil;
  NSUInteger (^block)(void) = ^{
    if (offset & 1)
      return array.count + offset;
    return array.count + other.count + offset;
  };
#if __has_feature(objc_arc)
  return block;
#else
  return [(id)Block_copy(block) autorelease];
#endif
}
- (void *)duplicateBlock:(void *)block {
  return _Block_copy(block);
}
- (void)releaseBlock:(void *)block {
  _Block_release(block);
}
- (NSUInteger)copiedCountForArray:(NSArray *)array offset:(NSUInteger)offset {
  void *owned = _Block_copy((__bridge const void *)^{
    return array.count + offset;
  });
  NSUInteger result;
  if (array.count & 1)
    result = ((__bridge NSUInteger(^)(void))owned)();
  else
    result = offset ^ 0x55;
  _Block_release(owned);
  return result;
}
- (void)asynchronouslyAppend:(id)value toArray:(NSMutableArray *)array
                       queue:(dispatch_queue_t)queue {
  dispatch_async(queue, ^{ [array addObject:value]; });
}
- (void)barrierAppend:(id)value toArray:(NSMutableArray *)array
                queue:(dispatch_queue_t)queue {
  dispatch_barrier_async(queue, ^{ [array addObject:value]; });
}
- (void)synchronouslyAppend:(id)value
                    toArray:(NSMutableArray *)array
                      queue:(dispatch_queue_t)queue {
  dispatch_sync(queue, ^{
    [array addObject:value];
  });
}
#if !__has_feature(objc_arc)
- (id (^)(void))holderForBlock:(NSUInteger (^)(void))block {
  id (^holder)(void) = ^id {
    return block;
  };
  return [(id)Block_copy(holder) autorelease];
}
#endif
@end
