//===- driver_kmdf_interrupt_test.h - Interrupt fixture contract ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DRIVER_KMDF_INTERRUPT_TEST_H
#define NEVERD_DRIVER_KMDF_INTERRUPT_TEST_H

enum {
  KmdfInterruptLine = 'f',
  KmdfInterruptMsi = 'g',
  KmdfInterruptPassive = 'h',
  KmdfInterruptEnableFailure = 'i',
  KmdfInterruptPrepare = 'j',
  KmdfInterruptSerialization = 'k',
  KmdfInterruptPassiveCleanup = 'l',
  KmdfInterruptPassiveMsi = 'm',
  KmdfInterruptPassivePowerWait = 'w',
  KmdfInterruptVector = 0x91,
  KmdfInterruptIrql = 5,
  KmdfInterruptMessages = 2,
  KmdfInterruptSyncResult = 0x81,
  KmdfInterruptSnapshotWords = 4,
  KmdfInterruptDelay100ns = 2,
  KmdfInterruptPulse100ns = 7,
  KmdfInterruptPowerDelay100ns = 8,
};

#endif
