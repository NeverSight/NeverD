//===- Arm64eOrderedAccess.h - Ordered instruction observations -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The freestanding Arm model and the Darwin process supply the abort state
/// for the same independently assembled and lifted instruction comparisons.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIAGNOSTICS_ARM64E_ORDERED_ACCESS_H
#define NEVERD_DIAGNOSTICS_ARM64E_ORDERED_ACCESS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint64_t ESR, FAR, Word;
} OrderedAbort;

uint64_t accessOrdered(uint32_t Word, uintptr_t Base, unsigned Lane,
                       bool Original);
bool observeOrdered(uint32_t Word, uintptr_t Base, unsigned Lane, bool Original,
                    bool MayFault, uint64_t *Result, OrderedAbort *Abort);
int compareOrdered(unsigned Mode, unsigned EL, bool WithSIMD);

#endif // NEVERD_DIAGNOSTICS_ARM64E_ORDERED_ACCESS_H
