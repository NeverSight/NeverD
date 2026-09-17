#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern const unsigned char nd_string_metadata[] __asm__("_$sSSN");
extern const unsigned char nd_int_metadata[] __asm__("_$sSiN");

@interface NDSwiftRuntimeCalls : NSObject
- (const void *)stringMetadata;
- (const void *)integerMetadata;
- (void *)keep:(void *)object;
- (void)drop:(void *)object;
- (void *)weakInitialize:(void *)reference object:(void *)object;
- (void *)weakAssign:(void *)reference object:(void *)object;
- (void *)weakRead:(void *)reference;
- (void)weakDestroy:(void *)reference;
- (void *)objectType:(void *)object;
- (void)begin:(void *)address scratch:(void *)scratch flags:(uintptr_t)flags;
- (void)end:(void *)scratch;
@end

static unsigned destroyed;
@interface NDRuntimeValue : NSObject
@end
@implementation NDRuntimeValue
- (void)dealloc {
  ++destroyed;
  [super dealloc];
}
@end

#ifdef NEVERD_RECOVERED_ARC
#include "replacements.h"
#endif

static void checked(int value, unsigned line) {
  if (!value) {
    fprintf(stderr, "Swift runtime check failed at line %u\n", line);
    abort();
  }
}
#define check(value) checked(!!(value), __LINE__)

int main(void) {
  @autoreleasepool {
#ifdef NEVERD_RECOVERED_ARC
    installRecovered();
#endif
    NDSwiftRuntimeCalls *calls = [NDSwiftRuntimeCalls new];
    for (unsigned i = 0; i < 64; ++i) {
      check([calls stringMetadata] == nd_string_metadata);
      check([calls integerMetadata] == nd_int_metadata);
      check([calls stringMetadata] != [calls integerMetadata]);
      // Runtime storage is opaque to these methods. Supply aligned space for
      // the runtime to initialize, and verify its actual object-lifetime
      // effects.
      uintptr_t reference[8] = {0}, scratch[8] = {0}, value = 0;
      NDRuntimeValue *first = [NDRuntimeValue new];
      NDRuntimeValue *second = [NDRuntimeValue new];
      void *metadata = [calls objectType:first];
      check(metadata != 0 && metadata == [calls objectType:second]);
      check(metadata != [calls objectType:calls]);
      check([calls keep:first] == first);
      [first release];
      check(destroyed == 2 * i);
      check([calls weakInitialize:reference object:first] == reference);
      check([calls weakRead:reference] == first);
      [calls drop:first]; // Release the load's +1.
      check([calls weakAssign:reference object:second] == reference);
      [calls drop:first]; // The explicit keep is now the last strong owner.
      check(destroyed == 2 * i + 1);
      check([calls weakRead:reference] == second);
      [calls drop:second];
      [second release];
      check(destroyed == 2 * i + 2);
      check([calls weakRead:reference] == 0);
      [calls weakDestroy:reference];
      [calls begin:&value scratch:scratch flags:0x20];
      check(value == 0);
      [calls end:scratch];
    }
    [calls release];
    printf("swift-runtime=pass\nweak=pass\naccess=pass\ndestroyed=%u\n",
           destroyed);
  }
}
