#import <Foundation/Foundation.h>

static id NDMutableString = @"shared initial string";
static id NDIndependentString = @"shared initial string";

@interface NDMutableConstants : NSObject
- (id)value;
- (id)independent;
- (id)initial;
- (void)setValue:(id)value;
- (void)setIndependent:(id)value;
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
@end
