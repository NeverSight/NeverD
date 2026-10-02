//===- windows_module_process.c - Original EXE -> DLL -> DLL oracle ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsModuleFixture.h"
__declspec(dllimport) DWORD MiddleProbe(void);
__declspec(dllimport) ULONG_PTR MiddleAPIGate(void);
__declspec(dllimport) DWORD LeafValue;
__declspec(dllimport) DWORD MiddleValue;
__declspec(dllimport) void ExitProcess(DWORD);
__declspec(dllimport) void *GetModuleHandleW(const WCHAR *);
__declspec(dllimport) void *GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(void *, const void *, DWORD, DWORD *,
                                    void *);
typedef struct {
  void *Base, *AllocationBase;
  DWORD AllocationProtection;
  unsigned short Partition;
  ULONG_PTR Size;
  DWORD State, Protection, Type;
} MemoryInfo;
__declspec(dllimport) ULONG_PTR VirtualQuery(const void *, MemoryInfo *,
                                             ULONG_PTR);
static volatile DWORD Phase;
__declspec(thread) DWORD ThreadValue = TLSSeed;
static void check(int OK, DWORD Line) {
  if (!OK) {
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), &Line, sizeof(Line), &Written, 0);
    ExitProcess(FailureStatus);
  }
}
#define CHECK(Condition) check(!!(Condition), __LINE__)
static void output(const char *Bytes, DWORD Size) {
  DWORD Written;
  CHECK(WriteFile(GetStdHandle(StdoutSelector), Bytes, Size, &Written, 0));
  CHECK(Written == Size);
}
static unsigned char *teb(void) {
  unsigned char *Result;
#define NEVERD_MODULE_ASM(Name, Text) __asm__(Text : "=r"(Result));
#include "WindowsModuleCases.def"
#undef NEVERD_MODULE_ASM
  return Result;
}
static unsigned char *module(const WCHAR *Name, const void *Data) {
  SetLastError(LastErrorSeed);
  unsigned char *Base = GetModuleHandleW(Name);
  CHECK(Base && GetLastError() == LastErrorSeed);
  MemoryInfo Info;
  CHECK(VirtualQuery(Data, &Info, sizeof(Info)) == sizeof(Info));
  CHECK(Info.AllocationBase == Base && Info.State == MemCommit &&
        Info.Type == MemImage);
  return Base;
}
static void loaderLists(void *Leaf, void *Middle) {
  unsigned char *PEB = *(unsigned char **)(teb() + TebPEB);
  unsigned char *Ldr = *(unsigned char **)(PEB + PebLdr);
  const DWORD Lists[] = {LdrLoadList, LdrMemoryList, LdrInitList};
  const DWORD Links[] = {0, sizeof(void *) * 2, sizeof(void *) * 4};
  for (DWORD I = 0; I < sizeof(Lists) / sizeof(Lists[0]); ++I) {
    unsigned char *Head = Ldr + Lists[I];
    unsigned char *Previous = Head;
    DWORD Found = 0, Count = 0;
    for (unsigned char *Node = *(unsigned char **)Head; Node != Head;
         Node = *(unsigned char **)Node) {
      CHECK(++Count < MaxModules && ((void **)Node)[1] == Previous);
      unsigned char *Entry = Node - Links[I];
      void *Base = *(void **)(Entry + ModuleBase);
      if (Base == Leaf || Base == Middle) {
        CHECK(!*(void **)(Entry + ModuleEntry));
        CHECK(*(DWORD *)(Entry + ModuleSize));
        CHECK(*(WCHAR **)(Entry + ModuleName + UnicodeBuffer));
        Found |= Base == Leaf ? 1 : 2;
      }
      Previous = Node;
    }
    CHECK(((void **)Head)[1] == Previous && Found == 3);
  }
}
static void tls(void *Image, DWORD Reason, void *Reserved) {
  (void)Image;
  (void)Reserved;
  if (Reason == 1) {
    CHECK(!Phase && ThreadValue == TLSSeed);
    CHECK(MiddleProbe() == LeafSeed + MiddleSeed);
    ++LeafValue;
    ++ThreadValue;
    Phase = 1;
  } else if (!Reason) {
    CHECK(Phase == 2 && ThreadValue == TLSSeed + 1);
    CHECK(MiddleProbe() == LeafSeed + MiddleSeed + 2);
    output(Detached, sizeof(Detached) - 1);
    Phase = 3;
  }
}
__declspec(allocate(".tls")) char TLSStart;
__declspec(allocate(".tls$ZZZ")) char TLSEnd;
DWORD _tls_index;
typedef void (*TLSCallback)(void *, DWORD, void *);
static TLSCallback Callbacks[] = {tls, 0};
__declspec(allocate(".rdata")) const struct {
  const void *Start, *End;
  DWORD *Index;
  const TLSCallback *Callbacks;
  DWORD ZeroFill, Characteristics;
} _tls_used = {&TLSStart, &TLSEnd, &_tls_index, Callbacks, 0, 0};
DWORD entry(void) {
  CHECK(Phase == 1 && ThreadValue == TLSSeed + 1);
  CHECK(LeafValue == LeafSeed + 1 && MiddleValue == MiddleSeed);
  ++MiddleValue;
  CHECK(MiddleProbe() == LeafSeed + MiddleSeed + 2);
  CHECK(MiddleAPIGate() == (ULONG_PTR)&GetCurrentProcessId);
  void *Leaf = module(LeafName, &LeafValue);
  void *Middle = module(MiddleName, &MiddleValue);
  CHECK(Leaf != Middle && GetModuleHandleW(0));
  loaderLists(Leaf, Middle);
  CHECK(!GetModuleHandleW(MissingName) &&
        GetLastError() == ErrorModuleNotFound);
  output(Message, sizeof(Message) - 1);
  Phase = 2;
  ExitProcess(ExitStatus);
  return FailureStatus;
}
