// Self-contained execution fixture; no Foundation dependency or external code.
#include <objc/objc.h>

__attribute__((objc_root_class))
@interface NeverDObjCFixture {
  Class isa;
}
+ (int)classAnswer;
- (int)constant42;
- (int)echo:(int)value;
- (unsigned int)unsignedEcho:(unsigned int)value;
- (long long)wideEcho:(long long)value;
- (int)add:(int)left right:(int)right;
- (int)subtract:(int)left right:(int)right;
- (int)choose:(int)value;
- (int)sum:(const int *)values count:(int)count;
- (int)read:(const int *)value;
- (int)write:(int *)location value:(int)value;
- (int)combine:(int)first ignored:(int)unused last:(int)last;
- (id)selfValue;
- (SEL)commandValue;
@end

@implementation NeverDObjCFixture
+ (int)classAnswer {
  return 42;
}
- (int)constant42 {
  return 42;
}
- (int)echo:(int)value {
  return value;
}
- (unsigned int)unsignedEcho:(unsigned int)value {
  return value;
}
- (long long)wideEcho:(long long)value {
  return value;
}
- (int)add:(int)left right:(int)right {
  return left + right;
}
- (int)subtract:(int)left right:(int)right {
  return left - right;
}
- (int)choose:(int)value {
  if (value < 0)
    return value + 11;
  if (value > 7)
    return value - 3;
  return value + 5;
}
- (int)sum:(const int *)values count:(int)count {
  int result = 0;
  for (int index = 0; index < count; ++index)
    result += values[index];
  return result;
}
- (int)read:(const int *)value {
  return *value;
}
- (int)write:(int *)location value:(int)value {
  int previous = *location;
  *location = value;
  return previous;
}
- (int)combine:(int)first ignored:(int)unused last:(int)last {
  return first - last;
}
- (id)selfValue {
  return self;
}
- (SEL)commandValue {
  return _cmd;
}
@end
