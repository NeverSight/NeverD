// Self-owned executable corpus for Objective-C dispatch and object layout.
#include <objc/objc.h>

typedef int (^NDUnaryBlock)(int);

__attribute__((objc_root_class))
@interface NDCallBase {
  Class isa;
@protected
  int _bias;
  long long _wide;
  id _peer;
}
+ (int)classStep:(int)value;
- (int)leaf:(int)value;
- (void)setBias:(int)value;
- (int)bias;
- (void)setWide:(long long)value;
- (long long)wide;
- (id)exchangePeer:(id)value;
@end

@interface NDCallChild : NDCallBase {
  int _extra;
}
+ (int)classStep:(int)value;
+ (int)selfClassSend:(int)value;
- (int)leaf:(int)value;
- (int)selfSend:(int)value;
- (int)objectSend:(NDCallBase *)object value:(int)value;
- (int)classSend:(int)value;
- (int)superSend:(int)value;
- (int)cHelper:(int)value;
- (int)adjustExtra:(int)value;
- (int)extra;
- (int)blockApply:(NDUnaryBlock)block value:(int)value;
- (int)capturedBlock:(int)value;
- (int)globalBlock:(int)value;
@end

@interface NDCallChild (Extras)
- (int)categoryStep:(int)value;
@end

// This marker separates declarations for the independent execution harness.
// NEVERD_CALLS_IMPLEMENTATION
__attribute__((noinline)) int NDIntHelper(int value, int extra) {
  return value * 3 - extra;
}

// Keep actual stack/global Block objects and indirect invokes in the image;
// interprocedural specialization would otherwise erase the global Block case.
__attribute__((noinline, optnone)) int NDApplyBlock(NDUnaryBlock block, int value) {
  return block(value);
}

@implementation NDCallBase
+ (int)classStep:(int)value {
  return value + 13;
}
- (int)leaf:(int)value {
  return value + _bias;
}
- (void)setBias:(int)value {
  _bias = value;
}
- (int)bias {
  return _bias;
}
- (void)setWide:(long long)value {
  _wide = value;
}
- (long long)wide {
  return _wide;
}
- (id)exchangePeer:(id)value {
  id previous = _peer;
  _peer = value;
  return previous;
}
@end

@implementation NDCallChild
+ (int)classStep:(int)value {
  return value + 23;
}
+ (int)selfClassSend:(int)value {
  return [self classStep:value] + 9;
}
- (int)leaf:(int)value {
  return value + _bias + _extra;
}
- (int)selfSend:(int)value {
  return [self leaf:value] + 1;
}
- (int)objectSend:(NDCallBase *)object value:(int)value {
  return [object leaf:value] - 2;
}
- (int)classSend:(int)value {
  return [NDCallBase classStep:value] + 7;
}
- (int)superSend:(int)value {
  return [super leaf:value] + 11;
}
- (int)cHelper:(int)value {
  return NDIntHelper(value, _extra);
}
- (int)adjustExtra:(int)value {
  int previous = _extra;
  _extra = value;
  return previous;
}
- (int)extra {
  return _extra;
}
- (int)blockApply:(NDUnaryBlock)block value:(int)value {
  return block(value) + 4;
}
- (int)capturedBlock:(int)value {
  int captured = _extra;
  return NDApplyBlock(^(int operand) { return operand + captured; }, value) + 2;
}
- (int)globalBlock:(int)value {
  return NDApplyBlock(^(int operand) { return operand - 9; }, value) + 6;
}
@end

@implementation NDCallChild (Extras)
- (int)categoryStep:(int)value {
  return [self leaf:value] + 29;
}
@end
