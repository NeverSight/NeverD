#import <Foundation/Foundation.h>
#include <stdint.h>
typedef struct {
  void *object;
  void *storage;
} NDBoxPair;
typedef struct {
  const void *metadata;
  uintptr_t state;
} NDMetadataResponse;
extern NDBoxPair __attribute__((swiftcall)) swift_allocBox(const void *);
extern NDMetadataResponse __attribute__((swiftcall))
swift_checkMetadataState(uintptr_t, const void *);
@interface NDNativeRecordResults : NSObject
- (void *)newBox:(const void *)metadata
           value:(NSUInteger)value
         storage:(void **)storage;
- (NSUInteger)metadataState:(const void *)metadata
                    request:(NSUInteger)request
                     result:(const void **)result;
- (NSUInteger)unionStart:(NSUInteger)a
                  length:(NSUInteger)b
              otherStart:(NSUInteger)c
                  length:(NSUInteger)d
                  output:(NSUInteger *)output;
- (NSUInteger)intersectionStart:(NSUInteger)a
                         length:(NSUInteger)b
                     otherStart:(NSUInteger)c
                         length:(NSUInteger)d
                         output:(NSUInteger *)output;
@end
static __attribute__((noinline)) void *
makeBox(const void *metadata, NSUInteger value, void **storage) {
  NDBoxPair pair = swift_allocBox(metadata);
  *(uintptr_t *)pair.storage = value;
  *storage = pair.storage;
  return pair.object;
}
static __attribute__((noinline)) NSUInteger readState(const void *metadata,
                                                      NSUInteger request,
                                                      const void **result) {
  NDMetadataResponse response = swift_checkMetadataState(request, metadata);
  *result = response.metadata;
  return response.state;
}
static __attribute__((noinline)) NSUInteger
unionRangeWords(NSUInteger a, NSUInteger b, NSUInteger c, NSUInteger d,
                NSUInteger *output) {
  NSRange value = NSUnionRange(NSMakeRange(a, b), NSMakeRange(c, d));
  output[0] = value.length;
  ++output[1];
  return value.location;
}
static __attribute__((noinline)) NSUInteger
intersectionRangeWords(NSUInteger a, NSUInteger b, NSUInteger c, NSUInteger d,
                       NSUInteger *output) {
  NSRange value = NSIntersectionRange(NSMakeRange(a, b), NSMakeRange(c, d));
  output[0] = value.location;
  ++output[1];
  return value.length;
}
@implementation NDNativeRecordResults
- (void *)newBox:(const void *)metadata
           value:(NSUInteger)value
         storage:(void **)storage {
  return makeBox(metadata, value, storage);
}
- (NSUInteger)metadataState:(const void *)metadata
                    request:(NSUInteger)request
                     result:(const void **)result {
  return readState(metadata, request, result);
}
- (NSUInteger)unionStart:(NSUInteger)a
                  length:(NSUInteger)b
              otherStart:(NSUInteger)c
                  length:(NSUInteger)d
                  output:(NSUInteger *)output {
  return unionRangeWords(a, b, c, d, output);
}
- (NSUInteger)intersectionStart:(NSUInteger)a
                         length:(NSUInteger)b
                     otherStart:(NSUInteger)c
                         length:(NSUInteger)d
                         output:(NSUInteger *)output {
  return intersectionRangeWords(a, b, c, d, output);
}
@end
