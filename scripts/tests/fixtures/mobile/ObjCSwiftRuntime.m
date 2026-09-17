#import <Foundation/Foundation.h>
#include <stdint.h>

extern void *swift_unknownObjectRetain(void *);
extern void swift_unknownObjectRelease(void *);
extern void *swift_unknownObjectWeakInit(void *, void *);
extern void *swift_unknownObjectWeakAssign(void *, void *);
extern void *swift_unknownObjectWeakLoadStrong(void *);
extern void swift_unknownObjectWeakDestroy(void *);
extern void *swift_getObjectType(void *);
extern void swift_beginAccess(void *, void *, uintptr_t, void *);
extern void swift_endAccess(void *);
extern const unsigned char nd_string_metadata[] __asm__("_$sSSN");
extern const unsigned char nd_int_metadata[] __asm__("_$sSiN");

@interface NDSwiftRuntimeCalls : NSObject
@end
@implementation NDSwiftRuntimeCalls
- (const void *)stringMetadata {
  return nd_string_metadata;
}
- (const void *)integerMetadata {
  return nd_int_metadata;
}
- (void *)keep:(void *)object {
  return swift_unknownObjectRetain(object);
}
- (void)drop:(void *)object {
  swift_unknownObjectRelease(object);
}
- (void *)weakInitialize:(void *)reference object:(void *)object {
  return swift_unknownObjectWeakInit(reference, object);
}
- (void *)weakAssign:(void *)reference object:(void *)object {
  return swift_unknownObjectWeakAssign(reference, object);
}
- (void *)weakRead:(void *)reference {
  return swift_unknownObjectWeakLoadStrong(reference);
}
- (void)weakDestroy:(void *)reference {
  swift_unknownObjectWeakDestroy(reference);
}
- (void *)objectType:(void *)object {
  return swift_getObjectType(object);
}
- (void)begin:(void *)address scratch:(void *)scratch flags:(uintptr_t)flags {
  swift_beginAccess(address, scratch, flags, 0);
}
- (void)end:(void *)scratch {
  swift_endAccess(scratch);
}
@end
