//===- windows_dynamic_top.c - Original demand forwarder owner -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsDynamicFixture.h"
int dllEntry(void *Base, DWORD Reason, void *Reserved) {
  trace(TopRole, DLLKind, Reason, Reserved);
  visibility(TopRole, Base);
  return 1;
}
