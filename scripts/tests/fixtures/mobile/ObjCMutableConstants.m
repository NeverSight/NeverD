#import <Foundation/Foundation.h>

static id NDMutableString = @"shared initial string";
static id NDIndependentString = @"shared initial string";
static void *NDManualValue;
extern void *NDKeepValue(void *) __asm__("_objc_retain");
extern void NDDropValue(void *) __asm__("_objc_release");

@interface NDMutableConstants : NSObject
- (id)value;
- (id)independent;
- (id)initial;
- (void)setValue:(id)value;
- (void)setIndependent:(id)value;
- (id)manualValue;
- (void)setManualValue:(void *)value;
@end

@implementation NDMutableConstants
- (id)value {
  return NDMutableString;
}
- (id)independent {
  return NDIndependentString;
}
- (id)initial {
  return @"shared initial string";
}
- (void)setValue:(id)value {
  NDMutableString = value;
}
- (void)setIndependent:(id)value {
  NDIndependentString = value;
}
- (id)manualValue {
  return (__bridge id)NDManualValue;
}
- (void)setManualValue:(void *)value {
  // Exercise an explicit pointer store as well as objc_storeStrong's typed
  // storage argument. The incoming pointer keeps its source parameter type.
  NDKeepValue(value);
  void *previous = NDManualValue;
  NDManualValue = value;
  NDDropValue(previous);
}
@end
