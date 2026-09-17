#import <Foundation/Foundation.h>

@interface NDScalarConstants : NSObject
- (double)finiteDouble;
- (double)negativeZeroDouble;
- (double)payloadDouble;
- (float)finiteFloat;
- (float)negativeZeroFloat;
- (float)payloadFloat;
- (void)fillWide:(void *)buffer;
@end
