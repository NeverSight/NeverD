//===- windows_process.c - Original freestanding Windows process --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// No Windows headers, CRT or redistributed system binaries are required.
typedef unsigned int DWORD;
typedef unsigned long long ULONG_PTR;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define NEVERD_WINDOWS_FIXTURE_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_WINDOWS_FIXTURE_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_WINDOWS_FIXTURE_BYTES(Name, ...)                                \
  __declspec(allocate(".text")) static const unsigned char Name[] = {          \
      __VA_ARGS__};
#include "WindowsProcessCases.def"
#undef NEVERD_WINDOWS_FIXTURE_BYTES
#undef NEVERD_WINDOWS_FIXTURE_TEXT
#undef NEVERD_WINDOWS_FIXTURE_VALUE
__declspec(dllimport) void ExitProcess(DWORD Status);
__declspec(dllimport) HANDLE GetStdHandle(DWORD Selector);
__declspec(dllimport) int WriteFile(HANDLE File, const void *Bytes, DWORD Size,
                                    DWORD *Written, void *Overlapped);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD Error);
__declspec(dllimport) DWORD GetCurrentProcessId(void);
__declspec(dllimport) DWORD GetCurrentThreadId(void);
__declspec(dllimport) WCHAR *GetCommandLineW(void);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(HANDLE Heap, DWORD Flags, ULONG_PTR Size);
__declspec(dllimport) int HeapFree(HANDLE Heap, DWORD Flags, void *Address);
__declspec(dllimport) ULONG_PTR HeapSize(HANDLE Heap, DWORD Flags,
                                         void *Address);
__declspec(dllimport) DWORD TlsAlloc(void);
__declspec(dllimport) int TlsFree(DWORD Index);
__declspec(dllimport) int TlsSetValue(DWORD Index, void *Value);
__declspec(dllimport) void *TlsGetValue(DWORD Index);
__declspec(dllimport) HANDLE GetModuleHandleW(const WCHAR *Name);

__attribute__((naked)) static void tailExit(void) {
#define NEVERD_WINDOWS_FIXTURE_ASM(Name, Text) NEVERD_WINDOWS_ASM_##Name(Text)
#define NEVERD_WINDOWS_ASM_TailExitCode(Text) __asm__(Text);
#define NEVERD_WINDOWS_ASM_ReadTEB(Text)
#define NEVERD_WINDOWS_ASM_ReturnSlotCode(Text)
#include "WindowsProcessCases.def"
#undef NEVERD_WINDOWS_ASM_ReturnSlotCode
#undef NEVERD_WINDOWS_ASM_ReadTEB
#undef NEVERD_WINDOWS_ASM_TailExitCode
#undef NEVERD_WINDOWS_FIXTURE_ASM
}
__attribute__((naked)) static void overwriteReturn(HANDLE File,
                                                   const void *Bytes,
                                                   DWORD Count, DWORD *Written,
                                                   void *Overlapped) {
#define NEVERD_WINDOWS_FIXTURE_ASM(Name, Text) NEVERD_WINDOWS_ASM_##Name(Text)
#define NEVERD_WINDOWS_ASM_ReturnSlotCode(Text) __asm__(Text);
#define NEVERD_WINDOWS_ASM_ReadTEB(Text)
#define NEVERD_WINDOWS_ASM_TailExitCode(Text)
#include "WindowsProcessCases.def"
#undef NEVERD_WINDOWS_ASM_ReturnSlotCode
#undef NEVERD_WINDOWS_ASM_ReadTEB
#undef NEVERD_WINDOWS_ASM_TailExitCode
#undef NEVERD_WINDOWS_FIXTURE_ASM
}
__declspec(dllimport) void RtlExitUserProcess(DWORD Status);
__declspec(dllimport) HANDLE GetCurrentProcess(void);
__declspec(dllimport) HANDLE GetCurrentThread(void);
static unsigned char *teb(void) {
  unsigned char *Result;
#define NEVERD_WINDOWS_FIXTURE_ASM(Name, Text) NEVERD_WINDOWS_ASM_##Name(Text)
#define NEVERD_WINDOWS_ASM_ReadTEB(Text) __asm__(Text : "=r"(Result));
#define NEVERD_WINDOWS_ASM_TailExitCode(Text)
#define NEVERD_WINDOWS_ASM_ReturnSlotCode(Text)
#include "WindowsProcessCases.def"
#undef NEVERD_WINDOWS_ASM_ReturnSlotCode
#undef NEVERD_WINDOWS_ASM_ReadTEB
#undef NEVERD_WINDOWS_ASM_TailExitCode
#undef NEVERD_WINDOWS_FIXTURE_ASM
  return Result;
}
static void require(int OK) {
  if (!OK)
    ExitProcess(Failure);
}
#include "WindowsMemoryFixture.inc"
static char mode(void) {
  WCHAR *Line = GetCommandLineW();
  unsigned N = 0;
  while (Line[N])
    ++N;
  // Windows launchers may append delimiters after the final argument.
  while (N && (Line[N - 1] == ' ' || Line[N - 1] == '\t'))
    --N;
  const int Quoted = N && Line[N - 1] == '"';
  if (Quoted)
    --N;
  require(N != 0);
  unsigned Start = N - 1;
  if (Quoted) {
    require(Start && Line[Start - 1] == '"');
    --Start;
  }
  require(Start && (Line[Start - 1] == ' ' || Line[Start - 1] == '\t'));
  static const char *const Modes[] = {
      Normal,        Returned,    Loop,     InitLoop,    Fault,
      Privileged,    Unknown,     Errors,   Unsupported, BadOutput,
      AliasedOutput, TLSMutation, TailExit, ForgedGate,  ReentrantExit,
      ReturnSlot,    NativeExit,
#define NEVERD_WINDOWS_FIXTURE_TEXT(Name, Text) Name,
#include "WindowsMemoryCases.def"
#undef NEVERD_WINDOWS_FIXTURE_TEXT
  };
  for (unsigned I = 0; I < sizeof(Modes) / sizeof(Modes[0]); ++I)
    if (Line[N - 1] == (unsigned char)Modes[I][0])
      return Modes[I][0];
  require(0);
  return 0;
}
__declspec(thread) DWORD ThreadValue = TLSSeed;
__declspec(thread) DWORD ThreadZero;
__declspec(allocate(".tls")) char TLSStart;
__declspec(allocate(".tls$ZZZ")) char TLSEnd;
DWORD _tls_index;
static volatile DWORD Phase;
static void tlsSecond(void *, DWORD, void *);
static void tlsReplacement(void *, DWORD, void *);
typedef void (*TLSCallback)(void *, DWORD, void *);
static TLSCallback Callbacks[3];
static void tlsFirst(void *Image, DWORD Reason, void *Reserved) {
  (void)Image;
  (void)Reserved;
  if (Reason == 1) {
    require(!Phase && ThreadValue == TLSSeed && ThreadZero == 0);
    ++ThreadValue;
    ThreadZero = 1;
    Phase = 1;
    if (mode() == 'i')
      while (Phase) {
      }
    if (mode() == 'm')
      Callbacks[1] = tlsReplacement;
  } else if (Reason == 0) {
    // The first callback is also first during process detach.
    require(Phase == 3);
    Phase = 4;
    if (mode() == 'd')
      ExitProcess(Failure);
  }
}
static void tlsSecond(void *Image, DWORD Reason, void *Reserved) {
  (void)Image;
  (void)Reserved;
  if (Reason == 1) {
    require(Phase == 1 && ThreadValue == TLSSeed + 1 && ThreadZero == 1);
    ++ThreadValue;
    Phase = 2;
  } else if (Reason == 0) {
    require(Phase == 4);
    Phase = 5;
    DWORD Written;
    require(WriteFile(GetStdHandle(StdoutSelector), Detached,
                      sizeof(Detached) - 1, &Written, 0));
  }
}
static void tlsReplacement(void *Image, DWORD Reason, void *Reserved) {
  require(Reason == 1);
  tlsSecond(Image, Reason, Reserved);
  Phase = 6;
}
static TLSCallback Callbacks[3] = {tlsFirst, tlsSecond, 0};
__declspec(allocate(".rdata")) const struct {
  const void *Start, *End;
  DWORD *Index;
  const TLSCallback *Callbacks;
  DWORD ZeroFill, Characteristics;
} _tls_used = {&TLSStart, &TLSEnd, &_tls_index, Callbacks, 0, 0};

DWORD entry(void) {
  const char Mode = mode();
  require(Phase == (Mode == 'm' ? 6 : 2) && ThreadValue == TLSSeed + 2 &&
          ThreadZero == 1);
  Phase = 3;
  if (Mode == 'm')
    Callbacks[1] = tlsSecond;
  unsigned char *TEB = teb();
  unsigned char *PEB = *(unsigned char **)(TEB + TebPEB);
  unsigned char *Parameters = *(unsigned char **)(PEB + PebParameters);
  require(*(void **)(PEB + PebImage) == GetModuleHandleW(0));
  require(*(ULONG_PTR *)(TEB + TebProcessID) == GetCurrentProcessId());
  require(*(ULONG_PTR *)(TEB + TebThreadID) == GetCurrentThreadId());
  require(*(WCHAR **)(Parameters + ParamsCommandLineBuffer) ==
          GetCommandLineW());
  SetLastError(LastErrorSeed);
  require(*(DWORD *)(TEB + TebLastError) == LastErrorSeed);
  *(DWORD *)(TEB + TebLastError) = LastErrorSeed + 1;
  require(GetLastError() == LastErrorSeed + 1);
  DWORD TLS = TlsAlloc(), Other = TlsAlloc();
  require(TLS != Other && TLS != (DWORD)-1 && Other != (DWORD)-1);
  require(TlsGetValue(TLS) == 0 && GetLastError() == 0);
  require(TlsSetValue(TLS, (void *)(ULONG_PTR)TLSSeed));
  require(TlsGetValue(TLS) == (void *)(ULONG_PTR)TLSSeed);
  require(*(ULONG_PTR *)(TEB + TebTLSSlots + TLS * sizeof(ULONG_PTR)) ==
          TLSSeed);
  require(TlsGetValue(Other) == 0 && ThreadValue == TLSSeed + 2);
  require(TlsFree(TLS) && TlsFree(Other));
  require(TlsGetValue(TLS) == 0 && GetLastError() == 0);
  SetLastError(LastErrorSeed);
  require(TlsSetValue(TLS, (void *)(ULONG_PTR)TLSSeed));
  require(GetLastError() == LastErrorSeed);
  require(TlsGetValue(TLS) == (void *)(ULONG_PTR)TLSSeed);
  require(GetCurrentProcess() == (HANDLE)(ULONG_PTR)-1);
  require(GetCurrentThread() == (HANDLE)(ULONG_PTR)-2);
  HANDLE Heap = GetProcessHeap();
  unsigned char *Bytes = HeapAlloc(Heap, HeapZero, HeapBytes);
  require(Bytes != 0 && !((ULONG_PTR)Bytes & 15));
  for (unsigned I = 0; I < HeapBytes; ++I)
    require(Bytes[I] == 0);
  require(HeapSize(Heap, 0, Bytes) >= HeapBytes);
  Bytes[HeapBytes - 1] = 1;
  require(HeapFree(Heap, 0, Bytes));
  memoryScenario(Mode);
  if (Mode == 't')
    tailExit();
  if (Mode == 'g')
    ((void (*)(void))(ULONG_PTR)ForgedReturnGate)();
  if (Mode == 'l')
    while (Phase) {
    }
  if (Mode == 'f')
    *(volatile DWORD *)(ULONG_PTR)1 = Failure;
  if (Mode == 'u' || Mode == 'p') {
#ifdef _M_X64
    ((void (*)(void))(Mode == 'u' ? X64Syscall : X64Privileged))();
#else
    ((void (*)(void))(Mode == 'u' ? ARMSyscall : ARMPrivileged))();
#endif
  }
  DWORD Written = Failure;
  if (Mode == 'a') {
    Written = LastErrorSeed;
    require(WriteFile(GetStdHandle(StderrSelector), &Written, sizeof(Written),
                      &Written, 0));
    require(Written == sizeof(Written));
  }
  if (Mode == 'v')
    overwriteReturn(GetStdHandle(StdoutSelector), Message, 1, &Written, 0);
  if (Mode == 'e') {
    const int Success = WriteFile(GetStdHandle(StdoutSelector),
                                  (void *)(ULONG_PTR)1, 8, &Written, 0);
    const DWORD WriteError = GetLastError();
    const ULONG_PTR TLSValue = (ULONG_PTR)TlsGetValue((DWORD)-1);
    const DWORD TLSError = GetLastError();
    if (Success || Written || WriteError != InvalidUserBuffer || TLSValue ||
        TLSError != InvalidParameter) {
      const ULONG_PTR Observed[] = {(DWORD)Success, Written, WriteError,
                                    TLSValue, TLSError};
      DWORD Reported;
      WriteFile(GetStdHandle(StderrSelector), Observed, sizeof(Observed),
                &Reported, 0);
      require(0);
    }
  }
  if (Mode == 'b')
    WriteFile(GetStdHandle(StdoutSelector), Message, 1, (void *)(ULONG_PTR)1,
              0);
  if (Mode == 's')
    WriteFile(GetStdHandle(StdoutSelector), Message, 1, &Written,
              (void *)(ULONG_PTR)1);
  require(WriteFile(GetStdHandle(StdoutSelector), Message, sizeof(Message) - 1,
                    &Written, 0));
  require(Written == sizeof(Message) - 1);
  require(WriteFile(GetStdHandle(StderrSelector), Binary, sizeof(Binary) - 1,
                    &Written, 0));
  if (Mode == 'q')
    RtlExitUserProcess(ExitStatus);
  if (Mode == 'r')
    return ExitStatus;
  ExitProcess(ExitStatus);
  return Failure;
}
