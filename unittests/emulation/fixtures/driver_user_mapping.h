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

typedef struct DriverUserMappingRequest {
  unsigned char *Report;
} DriverUserMappingRequest;

#endif // NEVERD_TEST_DRIVER_USER_MAPPING_H
