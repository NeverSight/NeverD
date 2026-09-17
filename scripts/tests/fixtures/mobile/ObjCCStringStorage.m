#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#import <objc/runtime.h>

@interface NDCStringStorage : NSObject
- (const char *)label;
- (const char *)suffix;
- (dispatch_queue_t)newQueue;
- (NSString *)string;
- (id)stored;
- (void)setStored:(id)value;
@end

@implementation NDCStringStorage
- (NSString *)string {
  return @"persist-queue-label";
}
- (id)stored {
  return objc_getAssociatedObject(self, "persist-queue-label" + 8);
}
- (void)setStored:(id)value {
  objc_setAssociatedObject(self, "persist-queue-label" + 8, value,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
- (const char *)label {
  return "persist-queue-label";
}
- (const char *)suffix {
  return "persist-queue-label" + 8;
}
- (dispatch_queue_t)newQueue {
  return dispatch_queue_create("persist-queue-label", DISPATCH_QUEUE_SERIAL);
}
@end
