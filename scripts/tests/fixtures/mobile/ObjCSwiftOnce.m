#import <Foundation/Foundation.h>
#include <stdint.h>

extern void swift_once(uintptr_t *, void (*)(void *), void *);
static uintptr_t NDOncePredicate;
static uintptr_t NDOnceValue;
static uintptr_t NDOnceCount;

__attribute__((noinline)) static uintptr_t NDOnceSeed(uintptr_t count) {
  return count + 100;
}
static void NDOnceInitialize(void *context) {
  (void)context;
  NDOnceValue = NDOnceSeed(++NDOnceCount);
}
__attribute__((noinline, visibility("hidden"))) uintptr_t NDOnceGetter(
    uintptr_t *predicate, uintptr_t *storage, void (*initializer)(void *)) {
  if (*predicate != UINTPTR_MAX)
    swift_once(predicate, initializer, predicate);
  return *storage;
}

@interface NDSwiftOnce : NSObject
- (uintptr_t)value;
@end
@implementation NDSwiftOnce
- (uintptr_t)value {
  return NDOnceGetter(&NDOncePredicate, &NDOnceValue, NDOnceInitialize);
}
@end
