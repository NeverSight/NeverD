#import <Foundation/Foundation.h>
#include <errno.h>
#include <math.h>
#include <objc/objc-sync.h>
#include <os/log.h>
#include <pthread.h>

@interface NDDarwinDeclarations : NSObject
- (NSString *)nameOfClass:(Class)value;
- (Class)classNamed:(NSString *)value;
- (NSString *)nameOfSelector:(SEL)value;
- (SEL)selectorNamed:(NSString *)value;
- (int64_t)incrementWithLock:(pthread_mutex_t *)lock counter:(int64_t *)counter;
- (int64_t)incrementWithObject:(id)object counter:(int64_t *)counter;
- (int64_t)compare:(CFStringRef)left with:(CFStringRef)right;
- (uint64_t)time:(uint64_t)when delta:(int64_t)delta;
- (int)lastError;
- (double)remainder:(double)value divisor:(double)divisor;
- (NSString *)defaultMode;
- (const void *)modeStorage;
- (NSString *)descriptionKey;
- (void *)mainQueue;
- (const void *)timerType;
- (void *)defaultLog;
- (void *)disabledLog;
- (float)defaultPriority;
- (double)foundationVersion;
- (BOOL)belongs:(id)object to:(Class)cls;
- (BOOL)responds:(id)object selector:(SEL)selector;
@end
@implementation NDDarwinDeclarations
- (NSString *)nameOfClass:(Class)value {
  return NSStringFromClass(value);
}
- (Class)classNamed:(NSString *)value {
  return NSClassFromString(value);
}
- (NSString *)nameOfSelector:(SEL)value {
  return NSStringFromSelector(value);
}
- (SEL)selectorNamed:(NSString *)value {
  return NSSelectorFromString(value);
}
- (int64_t)incrementWithLock:(pthread_mutex_t *)lock
                     counter:(int64_t *)counter {
  pthread_mutex_lock(lock);
  int64_t result = ++*counter;
  pthread_mutex_unlock(lock);
  return result;
}
- (int64_t)incrementWithObject:(id)object counter:(int64_t *)counter {
  objc_sync_enter(object);
  int64_t result = ++*counter;
  objc_sync_exit(object);
  return result;
}
- (int64_t)compare:(CFStringRef)left with:(CFStringRef)right {
  return CFStringCompare(left, right, kCFCompareCaseInsensitive);
}
- (uint64_t)time:(uint64_t)when delta:(int64_t)delta {
  return dispatch_time(when, delta);
}
- (int)lastError {
  return errno;
}
- (double)remainder:(double)value divisor:(double)divisor {
  return fmod(value, divisor);
}
- (NSString *)defaultMode {
  return NSDefaultRunLoopMode;
}
- (const void *)modeStorage {
  return &NSDefaultRunLoopMode;
}
- (NSString *)descriptionKey {
  return NSLocalizedDescriptionKey;
}
- (void *)mainQueue {
  return (__bridge void *)dispatch_get_main_queue();
}
- (const void *)timerType {
  return DISPATCH_SOURCE_TYPE_TIMER;
}
- (void *)defaultLog {
  return (__bridge void *)OS_LOG_DEFAULT;
}
- (void *)disabledLog {
  return (__bridge void *)OS_LOG_DISABLED;
}
- (float)defaultPriority {
  return NSURLSessionTaskPriorityDefault;
}
- (double)foundationVersion {
  return NSFoundationVersionNumber;
}
- (BOOL)belongs:(id)object to:(Class)cls {
  return [object isKindOfClass:cls];
}
- (BOOL)responds:(id)object selector:(SEL)selector {
  return [object respondsToSelector:selector];
}
@end
