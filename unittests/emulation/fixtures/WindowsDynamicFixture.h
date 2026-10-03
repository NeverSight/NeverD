//===- WindowsDynamicFixture.h - Independent loader fixture ABI -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_WINDOWS_DYNAMIC_FIXTURE_H
#define NEVERD_WINDOWS_DYNAMIC_FIXTURE_H
typedef unsigned int DWORD;
typedef unsigned long long ULONG_PTR;
typedef unsigned short WCHAR;
#define NEVERD_DYNAMIC_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_DYNAMIC_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_DYNAMIC_WIDE(Name, Text) static const WCHAR Name[] = Text;
#include "WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_WIDE
#undef NEVERD_DYNAMIC_TEXT
#undef NEVERD_DYNAMIC_VALUE
__declspec(dllimport) void *LoadLibraryW(const WCHAR *);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) void *GetModuleHandleW(const WCHAR *);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD);
__declspec(dllimport) void *GetStdHandle(DWORD);
__declspec(dllimport) int WriteFile(void *, const void *, DWORD, DWORD *,
                                    void *);
__declspec(dllimport) __declspec(noreturn) void ExitProcess(DWORD);
__declspec(dllimport) const WCHAR *GetCommandLineW(void);
static void check(int OK, DWORD Line) {
  if (!OK) {
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), &Line, sizeof(Line), &Written, 0);
    ExitProcess(FailureStatus);
  }
}
#define CHECK(Condition) check(!!(Condition), __LINE__)
static void output(const void *Bytes, DWORD Size) {
  DWORD Written;
  CHECK(WriteFile(GetStdHandle(StdoutSelector), Bytes, Size, &Written, 0));
  CHECK(Written == Size);
}
static void observation(char Tag, DWORD Value) {
  output(&Tag, sizeof(Tag));
  output(&Value, sizeof(Value));
}
static DWORD mode(void) {
  const WCHAR *P = GetCommandLineW();
  for (; *P; ++P)
    if (*P == ModePrefix)
      return P[1];
  return 0;
}
static unsigned char *teb(void) {
  unsigned char *P;
#define NEVERD_DYNAMIC_ASM(Name, Text) __asm__(Text : "=r"(P));
#include "WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_ASM
  return P;
}
static ULONG_PTR loadPointer(const void *Base, DWORD Offset) {
  return *(const ULONG_PTR *)((const unsigned char *)Base + Offset);
}
static DWORD inList(ULONG_PTR Head, DWORD LinkOffset, void *Base) {
  ULONG_PTR Node = loadPointer((void *)Head, 0);
  for (DWORD I = 0; Node != Head; ++I) {
    CHECK(Node && I < MaxModules);
    if (loadPointer((void *)(Node - LinkOffset), ModuleBase) == (ULONG_PTR)Base)
      return 1;
    Node = loadPointer((void *)Node, 0);
  }
  return 0;
}
static void visibility(char Role, void *Base) {
  if (mode() != VisibilityMode)
    return;
  const WCHAR *Name = Role == LeafRole     ? LeafName
                      : Role == MiddleRole ? MiddleName
                                           : TopName;
  const ULONG_PTR Peb = loadPointer(teb(), TebPEB);
  const ULONG_PTR Ldr = loadPointer((void *)Peb, PebLdr);
  DWORD Mask = GetModuleHandleW(Name) == Base;
  Mask |= inList(Ldr + LdrLoadList, 0, Base) << 1;
  Mask |= inList(Ldr + LdrMemoryList, MemoryLink, Base) << 2;
  Mask |= inList(Ldr + LdrInitList, InitLink, Base) << 3;
  observation(VisibilityTag, Mask);
}
static void trace(char Role, char Kind, DWORD Reason, void *Reserved) {
  const char Bytes[] = {Role, Kind, ZeroDigit + (char)Reason,
                        ZeroDigit + (Reserved != 0), LineEnd};
  output(Bytes, sizeof(Bytes));
}
typedef DWORD (*ProbeFunction)(void);
static void probe(void *Module, DWORD Expected) {
  ProbeFunction F = (ProbeFunction)GetProcAddress(Module, ProbeName);
  CHECK(F && F() == Expected);
}
#ifdef NEVERD_DYNAMIC_TLS
static DWORD Sentinel = Seed;
__declspec(thread) DWORD ThreadValue = Seed;
__declspec(thread) DWORD *ThreadPointer = &Sentinel;
__declspec(thread) unsigned char ThreadZero[ZeroTail];
__declspec(allocate(".tls")) char TLSStart;
__declspec(allocate(".tls$ZZZ")) char TLSEnd;
DWORD _tls_index;
typedef void (*TLSCallback)(void *, DWORD, void *);
static void tls(void *, DWORD, void *);
static TLSCallback Callbacks[] = {tls, 0};
__declspec(allocate(".rdata")) const struct {
  const void *Start, *End;
  DWORD *Index;
  const TLSCallback *Callbacks;
  DWORD ZeroFill, Characteristics;
} _tls_used = {&TLSStart, &TLSEnd, &_tls_index, Callbacks, 0, 0};
static void checkTLS(DWORD Reason) {
  CHECK(ThreadPointer == &Sentinel && *ThreadPointer == Seed);
  if (Reason == AttachReason) {
    CHECK(ThreadValue == Seed);
    for (DWORD I = 0; I < ZeroTail; ++I)
      CHECK(!ThreadZero[I]);
  }
  ++ThreadValue;
}
#endif
#endif
