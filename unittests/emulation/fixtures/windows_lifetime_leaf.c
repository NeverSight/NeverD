//===- windows_lifetime_leaf.c - Original dependency TLS and entry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsLifetimeFixture.h"
__declspec(dllimport) DWORD FlsAlloc(void *);
__declspec(dllimport) int FlsFree(DWORD);
__declspec(dllimport) void *FlsGetValue(DWORD);
__declspec(dllimport) int FlsSetValue(DWORD, void *);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, DWORD, ULONG_PTR);
__declspec(dllimport) int HeapFree(void *, DWORD, void *);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
static DWORD FLSIndex, FLSChild, FLSAdded;
static void *FLSModule;
__declspec(dllexport) void PrepareFLSUnload(void) {
  FLSModule = LoadLibraryA("life-cleanup.dll");
  CHECK(FLSModule != 0);
}
static void flsChild(void *Value) {
  CHECK(Value == (void *)(Seed + 2) && FlsGetValue(FLSChild) == Value);
  CHECK(!FlsGetValue(FLSIndex));
  void *Heap = GetProcessHeap();
  ULONG_PTR *P = HeapAlloc(Heap, 0, sizeof(ULONG_PTR));
  CHECK(P != 0);
  *P = (ULONG_PTR)Value;
  CHECK(*P == Seed + 2 && HeapFree(Heap, 0, P));
  trace(LeafRole, FLSChildKind, 0, 0);
}
static void flsCleanup(void *Value) {
  CHECK(Value == (void *)Seed && FlsGetValue(FLSIndex) == Value);
  CHECK(FlsSetValue(FLSIndex, (void *)(Seed + 1)));
  CHECK(FlsSetValue(FLSChild, (void *)(Seed + 2)));
  FLSAdded = FlsAlloc((void *)flsChild);
  CHECK(FLSAdded != 0xffffffffU && FLSAdded > FLSChild);
  CHECK(FlsSetValue(FLSAdded, (void *)(Seed + 3)));
  trace(LeafRole, FLSKind, 0, 0);
  if (FLSModule)
    CHECK(FreeLibrary(FLSModule));
}
__declspec(dllexport) DWORD LeafIndex(void) {
  CHECK(ThreadPointer == &Sentinel && ThreadValue >= Seed);
  return _tls_index;
}
static void tls(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  checkTLS(Reason);
  trace(LeafRole, TLSKind, Reason, Reserved);
  if (Reason == AttachReason && mode() == FaultMode)
    *(volatile DWORD *)(ULONG_PTR)FaultAddress = Seed;
  if (Reason == AttachReason && mode() == ExitLeafTLSMode)
    ExitProcess(ExitStatus);
}
int dllEntry(void *Image, DWORD Reason, void *Reserved) {
  CHECK(Image != 0);
  trace(LeafRole, DLLKind, Reason, Reserved);
  // Process detach may follow thread teardown, after its TLS was released.
  if (Reason == AttachReason) {
    ++ThreadValue;
    if (mode() == FLSExitMode || mode() == FLSUnloadMode) {
      FLSIndex = FlsAlloc((void *)flsCleanup);
      CHECK(FLSIndex != 0xffffffffU && FlsSetValue(FLSIndex, (void *)Seed));
      FLSChild = FlsAlloc((void *)flsChild);
      CHECK(FLSChild != 0xffffffffU && FLSChild > FLSIndex);
    }
    if (mode() == ExitLeafEntryMode)
      ExitProcess(ExitStatus);
    return mode() != FailLeafMode;
  }
  if (Reason == DetachReason &&
      (mode() == FLSExitMode || mode() == FLSUnloadMode)) {
    CHECK(!FlsGetValue(FLSIndex) && !FlsGetValue(FLSChild));
    CHECK(FlsGetValue(FLSAdded) == (void *)(Seed + 3));
    CHECK(FlsSetValue(FLSIndex, 0) && FlsSetValue(FLSChild, 0));
    CHECK(FlsSetValue(FLSAdded, 0));
    CHECK(FlsFree(FLSIndex) && FlsFree(FLSChild) && FlsFree(FLSAdded));
  }
  return 0;
}
