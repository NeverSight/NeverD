#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

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

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static void checked(int value, unsigned line) {
  if (!value) {
    fprintf(stderr, "constant string check failed at line %u\n", line);
    abort();
  }
}
#define check(value) checked(!!(value), __LINE__)

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDConstantStrings *calls = [NDConstantStrings new];
    for (unsigned round = 0; round < 128; ++round) {
      check([[calls ascii] isEqualToString:@"sites\n\"quoted\""]);
      check([calls ascii] == [calls alias]);
      check([calls ascii] == [calls indirectASCII]);
      check([calls indirectASCII] == [calls indirectAlias]);
      check([calls unicode] == [calls indirectUnicode]);
      NSString *replacement = round & 1 ? @"odd" : @"even";
      [calls setMutableValue:replacement];
      check([calls mutableValue] == replacement);
      check([calls slotAddress] == &NDConstantASCII);
      check([[calls unicode] isEqualToString:@"百科😀"]);
      check([[calls unicode] length] == 4);
      check([[calls embedded] length] == 3);
      check([[calls embedded] characterAtIndex:1] == 0);
      check([[calls embedded] characterAtIndex:2] == 'b');
      check([[calls empty] length] == 0);
      check([calls first] != [calls second]);
      check([[calls first] isEqual:[calls second]]);
      for (NSString *value in @[
             [calls ascii], [calls unicode], [calls embedded], [calls empty],
             [calls first], [calls second], [calls indirectASCII],
             [calls indirectAlias], [calls indirectUnicode]
           ]) {
        check(object_getClass(value) == object_getClass(@"literal"));
        check([value retain] == value);
        [value release];
        check([value copy] == value);
        [value release];
      }
    }
    [calls release];
    puts("constant-strings=pass\nunicode=pass\nidentity=pass\nlifetime=pass");
  }
}
