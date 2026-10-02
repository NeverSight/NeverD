//===- windows_module_leaf.c - Original ordinal/data DLL ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsModuleFixture.h"
DWORD LeafValue = LeafSeed;
static DWORD *volatile Relocated = &LeafValue;
DWORD LeafOrdinal(void) {
  return GetCurrentProcessId() ? *Relocated : FailureStatus;
}
