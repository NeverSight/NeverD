//===- driver_user_mapping.h - User MDL mapping fixture protocol --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_DRIVER_USER_MAPPING_H
#define NEVERD_TEST_DRIVER_USER_MAPPING_H

enum DriverUserMappingAction {
  UserMappingPoolAliases = 0x222103,
  UserMappingAttachedWorker = 0x222107,
  UserMappingProcessExit = 0x22210b,
  UserMappingWrongProcessUnmap = 0x22210f,
  UserMappingShortage = 0x222113,
  UserMappingPartialReuse = 0x222117,
  UserMappingAddressReuse = 0x22211b,
  UserMappingRetainProcessView = 0x22211f,
  UserMappingConcurrentProcessView = 0x222123,
  UserMappingReleaseFirstProcessView = 0x222127,
  UserMappingReleaseSecondProcessView = 0x22212b,
  UserMappingIndependentPages = 0x22212f,
  UserMappingLegacyPages = 0x222133,
  UserMappingIndependentPartial = 0x222137,
  UserMappingContiguousChunks = 0x22213b,
  UserMappingKernelPoolLock = 0x22213f,
  UserMappingReportSize = 4,
  UserMappingByteOffset = 17,
  UserMappingFirstByte = 0x43,
  UserMappingSecondByte = 0x75,
  UserMappingWorkerByte = 0xa7,
  UserMappingPoolTag = 0x55766c4d
};

enum DriverPartialMappingChecks {
  PartialOriginalRange = 1,
  PartialPhysicalPages = 2,
  PartialPreparedDescriptor = 4,
  PartialReusedRange = 8,
  PartialInheritedMapping = 1,
  PartialInheritedPrepare = 2,
  PartialSourceReleased = 4,
  PartialDescendantSurvives = 8
};

enum DriverUserRemappingChecks {
  UserRemappingUnmappedFault = 1,
  UserRemappingReadOnlyFault = 2,
  UserRemappingReusedAddress = 4,
  UserRemappingPreservedLockedPages = 8
};

typedef struct DriverUserMappingRequest {
  unsigned char *Report;
} DriverUserMappingRequest;

#endif // NEVERD_TEST_DRIVER_USER_MAPPING_H
