//===- windows_module_middle.c - Original dependent no-entry DLL ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsModuleFixture.h"
__declspec(dllimport) DWORD LeafValue;
__declspec(dllimport) DWORD LeafOrdinal(void);
DWORD MiddleValue = MiddleSeed;
static DWORD *volatile Relocated = &MiddleValue;
DWORD MiddleProbe(void) {
  SetLastError(LastErrorSeed);
  return GetLastError() == LastErrorSeed && LeafOrdinal() == LeafValue
             ? LeafValue + *Relocated
             : FailureStatus;
}
ULONG_PTR MiddleAPIGate(void) { return (ULONG_PTR)&GetCurrentProcessId; }
