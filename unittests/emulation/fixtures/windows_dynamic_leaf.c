//===- windows_dynamic_leaf.c - Original loaded DLL ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#define NEVERD_DYNAMIC_TLS
#include "WindowsDynamicFixture.h"
__declspec(dllexport) DWORD Leaf(void) { return ThreadValue; }
static void tls(void *Base, DWORD Reason, void *Reserved) {
  checkTLS(Reason);
  trace(LeafRole, TLSKind, Reason, Reserved);
  visibility(LeafRole, Base);
}
int dllEntry(void *Base, DWORD Reason, void *Reserved) {
  if (Reason == AttachReason && mode() == ReentrantMode)
    LoadLibraryW(MiddleName);
  trace(LeafRole, DLLKind, Reason, Reserved);
  visibility(LeafRole, Base);
  return !(Reason == AttachReason &&
           (mode() == LeafRole || mode() == ForwardFailedLeafMode));
}
