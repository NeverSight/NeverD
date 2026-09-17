#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

static dispatch_once_t NDOnceToken;
static id NDSharedObject;
static NSUInteger NDInitializationCount;

@interface NDDispatchOnce : NSObject
+ (id)shared;
+ (NSUInteger)initializationCount;
@end
@implementation NDDispatchOnce
+ (id)shared {
  dispatch_once(&NDOnceToken, ^{
    ++NDInitializationCount;
    NDSharedObject = [self new];
  });
  return NDSharedObject;
}
+ (NSUInteger)initializationCount {
  return NDInitializationCount;
}
@end
