//===- driver_seh_test.h - Shared genuine C SEH fixture contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DRIVER_SEH_TEST_H
#define NEVERD_DRIVER_SEH_TEST_H

enum {
  SehXmmUnwind = 'x',
  SehChainedUnwind = 'c',
  SehPrologueUnwind = 'p',
  SehGSCookie = 'g',
  SehGSAlignedCookie = 'a',
  SehGSCorruptCookie = 'b',
  SehGSAlignedCorruptCookie = 'd',
  SehDynamicFilter = 'F',
  SehSearchFilters = 'Q',
  SehExceptionalFinally = 'T',
  SehNormalFinally = 'L',
  SehContinueApi = 'E',
  SehNestedFilter = 'B',
  SehNestedFinally = 'J',
  SehLocalFilter = 'I',
  SehLocalFinally = 'O',
  SehNestedSearch = 'P',
  SehRepeatedFilter = 'X',
  SehRecoverUserRead = 'V',
  SehRejectContextMutation = 'M',
  SehWorkerUserProbe = 'W',
  SehWorkerUserLock = 'K',
  SehAttachedWorker = 'Z',
  SehAttachedForbidden = 'Y',
  SehFilterUserMarker = 0x516e4a23,
  SehRecoverControlCode = 0x222403,
  SehRecoveredValue = 0x12345678
};
#endif
