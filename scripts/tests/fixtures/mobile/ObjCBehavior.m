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
- (float)floatIdentity:(float)value;
- (double)doubleIdentity:(double)value;
- (float)floatAdd:(float)left right:(float)right;
- (double)doubleAdd:(double)left right:(double)right;
- (double)mixed:(int)first
    doubleValue:(double)second
           wide:(long long)third
     floatValue:(float)fourth;
- (long long)manyIntegers:(long long)a
                        b:(long long)b
                        c:(long long)c
                        d:(long long)d
                        e:(long long)e
                        f:(long long)f
                        g:(long long)g
                        h:(long long)h;
- (double)manyDoubles:(double)a
                    b:(double)b
                    c:(double)c
                    d:(double)d
                    e:(double)e
                    f:(double)f
                    g:(double)g
                    h:(double)h
                    i:(double)i;
- (int)packedIntegers:(int)a
                    b:(int)b
                    c:(int)c
                    d:(int)d
                    e:(int)e
                    f:(int)f
                    g:(signed char)g
                    h:(short)h
                    i:(int)i;

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
- (float)floatIdentity:(float)value {
  return value;
}
- (double)doubleIdentity:(double)value {
  return value;
}
- (float)floatAdd:(float)left right:(float)right {
  return left + right;
}
- (double)doubleAdd:(double)left right:(double)right {
  return left + right;
}
- (double)mixed:(int)first
    doubleValue:(double)second
           wide:(long long)third
     floatValue:(float)fourth {
  return first + second + third + fourth;
}
- (long long)manyIntegers:(long long)a
                        b:(long long)b
                        c:(long long)c
                        d:(long long)d
                        e:(long long)e
                        f:(long long)f
                        g:(long long)g
                        h:(long long)h {
  return a + b + c + d + e + f + g + h;
}
- (double)manyDoubles:(double)a
                    b:(double)b
                    c:(double)c
                    d:(double)d
                    e:(double)e
                    f:(double)f
                    g:(double)g
                    h:(double)h
                    i:(double)i {
  return a + b + c + d + e + f + g + h + i;
}
- (int)packedIntegers:(int)a
                    b:(int)b
                    c:(int)c
                    d:(int)d
                    e:(int)e
                    f:(int)f
                    g:(signed char)g
                    h:(short)h
                    i:(int)i {
  return a + b + c + d + e + f + g + h + i;
}
@end
