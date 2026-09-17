#import "ObjCScalarConstants.h"

#include <stdint.h>
#include <string.h>

@implementation NDScalarConstants
- (double)finiteDouble {
  return 0x1.23456789abcdep+20;
}
- (double)negativeZeroDouble {
  return -0.0;
}
- (double)payloadDouble {
  return __builtin_nan("0x1234567");
}
- (float)finiteFloat {
  return 0x1.234568p+20f;
}
- (float)negativeZeroFloat {
  return -0.0f;
}
- (float)payloadFloat {
  return __builtin_nanf("0x12345");
}
- (void)fillWide:(void *)buffer {
  const uint64_t words[2] = {UINT64_C(0xfedcba9876543210),
                             UINT64_C(0x8123456789abcdef)};
  memcpy(buffer, words, sizeof(words));
}
@end
