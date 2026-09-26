//===- driver_nested_user.h - Nested user-memory fixture protocol --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_DRIVER_NESTED_USER_H
#define NEVERD_TEST_DRIVER_NESTED_USER_H

enum DriverNestedUserAction {
  NestedUserTransform = 0x222053,
  NestedUserLockedWorker = 0x222057,
  NestedFrameworkMode = 'k',
  NestedPayloadLength = 4,
  NestedPayloadOffset = 3,
  NestedResultOffset = 4,
  NestedBufferSize = 12,
  NestedTransformDelta = 0x30,
  NestedGuardByte = 0xa5
};

typedef struct DriverNestedBuffer {
  unsigned char *Data;
  unsigned char *Alias;
  unsigned int Length;
  unsigned int Reserved;
} DriverNestedBuffer;

typedef struct DriverNestedRequest {
  DriverNestedBuffer *Buffer;
  unsigned char *Result;
} DriverNestedRequest;

#endif // NEVERD_TEST_DRIVER_NESTED_USER_H
