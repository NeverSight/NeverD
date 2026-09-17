#import <Foundation/Foundation.h>
#include <stdint.h>

#ifdef NEVERD_NATIVE_POINTERS
extern void *NDKeepPointer(void *) __asm__("_objc_retain");

// Keep an ordinary stripped native ABI between literals and a typed consumer.
// Two different call sites prevent constant-argument specialization.
__attribute__((noinline, disable_tail_calls)) uintptr_t
NDForwardPointer(uintptr_t value) {
  return (uintptr_t)NDKeepPointer((void *)value);
}
#endif

extern int __CFConstantStringClassReference[];
static const char bytes[]
    __attribute__((section("__TEXT,__cstring,cstring_literals"))) = "same";
static const uint16_t wideBytes[]
    __attribute__((section("__TEXT,__ustring"))) = {'s', 'a', 'm', 'e', 0};
static struct {
  const void *isa;
  uint32_t flags;
  const void *bytes;
  int64_t length;
} first __attribute__((
    section("__DATA,__cfstring"))) = {__CFConstantStringClassReference, 0x7c8,
                                      bytes, 4},
  second __attribute__((section("__DATA,__cfstring"))) = {
      __CFConstantStringClassReference, 0x7d0, wideBytes, 4};

extern NSString *const NDConstantASCII;
extern NSString *const NDConstantAlias;
extern NSString *const NDConstantUnicode;
extern NSString *NDMutableString;

@interface NDConstantStrings : NSObject
- (NSString *)ascii;
- (NSString *)alias;
- (NSString *)unicode;
- (NSString *)embedded;
- (NSString *)empty;
- (NSString *)first;
- (NSString *)second;
- (NSString *)indirectASCII;
- (NSString *)indirectAlias;
- (NSString *)indirectUnicode;
- (NSString *)mutableValue;
- (void)setMutableValue:(NSString *)value;
- (const void *)slotAddress;
@end

@implementation NDConstantStrings
- (NSString *)indirectASCII {
  return NDConstantASCII;
}
- (NSString *)indirectAlias {
  return NDConstantAlias;
}
- (NSString *)indirectUnicode {
  return NDConstantUnicode;
}
- (NSString *)mutableValue {
  return NDMutableString;
}
- (void)setMutableValue:(NSString *)value {
  NDMutableString = value;
}
- (const void *)slotAddress {
  return &NDConstantASCII;
}
- (NSString *)ascii {
#ifdef NEVERD_NATIVE_POINTERS
  return (__bridge_transfer NSString *)(void *)NDForwardPointer(
      (uintptr_t)(__bridge void *)@"sites\n\"quoted\"");
#else
  return @"sites\n\"quoted\"";
#endif
}
- (NSString *)alias {
  return @"sites\n\"quoted\"";
}
- (NSString *)unicode {
#ifdef NEVERD_NATIVE_POINTERS
  return (__bridge_transfer NSString *)(void *)NDForwardPointer(
      (uintptr_t)(__bridge void *)@"百科😀");
#else
  return @"百科😀";
#endif
}
- (NSString *)embedded {
  return @"a\0b";
}
- (NSString *)empty {
  return @"";
}
- (NSString *)first {
  return (__bridge NSString *)(void *)&first;
}
- (NSString *)second {
  return (__bridge NSString *)(void *)&second;
}
@end
