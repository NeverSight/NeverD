//===- unpack_generated.c - A program whose loader writes its code --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
#define NEVERD_GENERATED_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_GENERATED_MODE(Name, Value) enum { Name##Mode = Value };
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_MODE
#undef NEVERD_GENERATED_VALUE
#define NEVERD_GENERATED_TEXT(Name, Text) static const char Name[] = Text;
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_TEXT
#include "unpack_pointer_state.h"
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) void *SetUnhandledExceptionFilter(void *);
__declspec(dllimport) void *GetModuleHandleA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) int GetSystemMetrics(int);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, U32, U64);
__declspec(dllimport) int HeapFree(void *, U32, void *);
#include "unpack_dynamic_tls_state.h"
#include "unpack_runtime_state.h"
volatile U32 *HeapState;
static volatile U64 EncodedState;

// Keep independently linked export lookup cells on both instruction sets.
// A generated loader may use them to ask a modeled service to write its code.
__declspec(dllexport) __attribute__((noinline)) void *
lookup_export(const char *Module, const char *Name) {
  return GetProcAddress(GetModuleHandleA(Module), Name);
}

// A protector can replace one six-byte import call with a register push and a
// call to one of these stubs. The stub drops that push and tail-calls the
// import, so the export returns to the instruction after the site. Nothing
// reaches them unless a test plants that call. The names stay in the export
// table so the test can aim the planted call.
#if defined(__x86_64__)
#define TAIL(Name)                                                             \
  __declspec(dllexport) __attribute__((naked, used)) void tail_##Name(void) {  \
    __asm__("push %rax\n\t"                                                    \
            "mov 8(%rsp), %rax\n\t"                                            \
            "mov %rax, 16(%rsp)\n\t"                                           \
            "pop %rax\n\t"                                                     \
            "lea 8(%rsp), %rsp\n\t"                                            \
            "jmp *__imp_" #Name "(%rip)");                                     \
  }
TAIL(ExitProcess)
TAIL(GetStdHandle)
TAIL(WriteFile)
#undef TAIL
#define PADDED_CALL(Name)                                                      \
  __declspec(dllexport) __attribute__((naked, used)) void call_##Name(void) {  \
    __asm__("push %rax\n\t"                                                    \
            "mov 8(%rsp), %rax\n\t"                                            \
            "lea 1(%rax), %rax\n\t"                                            \
            "mov %rax, 8(%rsp)\n\t"                                            \
            "pop %rax\n\t"                                                     \
            "jmp *__imp_" #Name "(%rip)");                                     \
  }
PADDED_CALL(ExitProcess)
PADDED_CALL(GetStdHandle)
PADDED_CALL(WriteFile)
PADDED_CALL(SetUnhandledExceptionFilter)
PADDED_CALL(GetSystemMetrics)
#undef PADDED_CALL
#endif

// The record a packer fills in; it is the only content of its section. As
// linked it is empty and the loader below is never reached.
struct PackRecord {
  U32 Mode, Key, ProgramBytes, RelayBytes;
  U8 Program[Capacity], Relay[Capacity];
};
#pragma section(".pay", read, write)
__declspec(allocate(".pay")) struct PackRecord Pack = {0};

// Each code section begins with the function a loader writes it for, so the
// address of that function is the address of the section.
#define PROGRAM_ENTRY __attribute__((section(".prog$a"), noinline))
#define PROGRAM_CODE __attribute__((section(".prog$m"), noinline))
#define RELAY_ENTRY __attribute__((section(".relay$a"), noinline))
typedef U32 (*Entry)(void);

#if defined(__x86_64__)
// The terminator deliberately cannot validate this live program cache as IAT.
volatile U64 CachedExport[] = {0, 0x5555555555555555ULL};
// This independently terminated cell precedes the static import section.
// Rebuilding it changes the order in which opaque exports receive addresses.
#pragma section(".cache", read, write)
__declspec(dllexport) __declspec(allocate(
    ".cache")) volatile U64 LateImports[] = {0, 0, 0x5555555555555555ULL};
volatile U32 AddressEffects;
__attribute__((noinline)) static void resolveLateImport(void) {
  if (!LateImports[0])
    LateImports[0] =
        (U64)GetProcAddress(GetModuleHandleA(OpaqueModule), LateExport);
}
__declspec(dllexport) __attribute__((naked, used)) void
late_export_helper(void) {
  __asm__("push %rax\n\t"
          "mov 8(%rsp), %rax\n\t"
          "lea 1(%rax), %rax\n\t"
          "mov %rax, 8(%rsp)\n\t"
          "pop %rax\n\t"
          "jmp *LateImports(%rip)");
}
__declspec(dllexport) __attribute__((naked, used)) void address_helper(void) {
  __asm__("push %rax\n\t"
          "mov 8(%rsp), %rax\n\t"
          "lea 1(%rax), %rax\n\t"
          "mov %rbx, 8(%rsp)\n\t"
          "mov CachedExport(%rip), %rbx\n\t"
          "xchg %rax, (%rsp)\n\t"
          "ret");
}
#define NEVERD_GENERATED_ADDRESS_REGISTER(Number)                              \
  __declspec(dllexport) __attribute__((naked, used)) void                      \
  address_helper_r##Number(void) {                                             \
    __asm__("push %rax\n\t"                                                    \
            "mov 8(%rsp), %rax\n\t"                                            \
            "lea 1(%rax), %rax\n\t"                                            \
            "mov %r" #Number ", 8(%rsp)\n\t"                                   \
            "mov CachedExport(%rip), %r" #Number "\n\t"                        \
            "xchg %rax, (%rsp)\n\t"                                            \
            "ret");                                                            \
  }
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_ADDRESS_REGISTER
#define NEVERD_GENERATED_ADDRESS_REGISTER(Number)                              \
  __declspec(dllexport) __attribute__((naked, used)) void                      \
  compact_helper_r##Number(void) {                                             \
    __asm__("push %rax\n\t"                                                    \
            "mov 8(%rsp), %rax\n\t"                                            \
            "mov %r" #Number ", 8(%rsp)\n\t"                                   \
            "mov CachedExport(%rip), %r" #Number "\n\t"                        \
            "xchg %rax, (%rsp)\n\t"                                            \
            "ret");                                                            \
  }
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_ADDRESS_REGISTER
__declspec(dllexport)
__attribute__((naked, used)) void call_address_helper(void) {
  __asm__("push %rax\n\t"
          "mov 8(%rsp), %rax\n\t"
          "lea 2(%rax), %rax\n\t"
          "mov %rax, 8(%rsp)\n\t"
          "mov CachedExport(%rip), %rbx\n\t"
          "pop %rax\n\t"
          "ret");
}
__declspec(dllexport) __attribute__((naked, used)) void
impure_tail_GetStdHandle(void) {
  __asm__("pushfq\n\t"
          "incl AddressEffects(%rip)\n\t"
          "popfq\n\t"
          "jmp tail_GetStdHandle");
}
__declspec(dllexport) __attribute__((naked, used)) void
impure_address_helper(void) {
  __asm__("pushfq\n\t"
          "incl AddressEffects(%rip)\n\t"
          "popfq\n\t"
          "jmp address_helper");
}
__declspec(dllexport) __attribute__((naked, used)) void
changing_address_helper(void) {
  __asm__("pushfq\n\t"
          "cmpl $0, AddressEffects(%rip)\n\t"
          "jne 1f\n\t"
          "popfq\n\t"
          "jmp address_helper\n"
          "1: popfq\n\t"
          "jmp impure_address_helper");
}
__declspec(dllexport) __attribute__((naked, used)) void
unresolved_address_helper(void) {
  __asm__("pushfq\n\t"
          "cmpl $0, AddressEffects(%rip)\n\t"
          "jne 1f\n\t"
          "popfq\n\t"
          "jmp address_helper\n"
          "1: popfq\n\t"
          "push %rax\n\t"
          "mov 8(%rsp), %rax\n\t"
          "lea 1(%rax), %rax\n\t"
          "mov %rbx, 8(%rsp)\n\t"
          "mov $0, %ebx\n\t"
          "xchg %rax, (%rsp)\n\t"
          "ret");
}
__declspec(dllexport) __attribute__((naked, used)) void
service_address_helper(void) {
  __asm__("pushfq\n\t"
          "push %rax\n\t"
          "push %rcx\n\t"
          "sub $40, %rsp\n\t"
          "mov $0xfffffff5, %ecx\n\t"
          "call *__imp_GetStdHandle(%rip)\n\t"
          "add $40, %rsp\n\t"
          "pop %rcx\n\t"
          "pop %rax\n\t"
          "popfq\n\t"
          "jmp address_helper");
}
PROGRAM_CODE __declspec(dllexport) __attribute__((naked, used)) U32
address_program(void) {
  // Register calls keep these separate address-load scenarios from adding
  // unreachable six-byte sites to the ordinary tail-call fixture.
  __asm__("push %rbx\n\t"
          "sub $32, %rsp\n\t"
          "mov __imp_GetStdHandle(%rip), %rbx\n\t"
          "mov $0xfffffff5, %ecx\n\t"
          "call *%rbx\n\t"
          "mov $43, %ecx\n\t"
          "mov __imp_ExitProcess(%rip), %rax\n\t"
          "call *%rax");
}
PROGRAM_CODE __declspec(dllexport) __attribute__((naked, used)) U32
address_twice(void) {
  __asm__("push %rbx\n\t"
          "sub $32, %rsp\n\t"
          "mov $2, %edi\n"
          "1: mov __imp_GetStdHandle(%rip), %rbx\n\t"
          "mov $0xfffffff5, %ecx\n\t"
          "call *%rbx\n\t"
          "movl $1, AddressEffects(%rip)\n\t"
          "dec %edi\n\t"
          "jne 1b\n\t"
          "mov $43, %ecx\n\t"
          "mov __imp_ExitProcess(%rip), %rax\n\t"
          "call *%rax");
}
PROGRAM_CODE __declspec(dllexport) __attribute__((naked, used)) U32
address_extended_program(void) {
  __asm__("sub $40, %rsp\n\t"
#define NEVERD_GENERATED_ADDRESS_REGISTER(Number)                              \
  "mov __imp_GetStdHandle(%rip), %r" #Number "\n\t"                            \
  "nop\n\t"                                                                    \
  "mov $0xfffffff5, %ecx\n\t"                                                  \
  "call *%r" #Number "\n\t"
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_ADDRESS_REGISTER
          "mov $43, %ecx\n\t"
          "mov __imp_ExitProcess(%rip), %rax\n\t"
          "call *%rax");
}
PROGRAM_CODE __declspec(dllexport) __attribute__((naked, used)) U32
opaque_program(void) {
  __asm__("sub $40, %rsp\n\t"
          "xor %ecx, %ecx\n\t"
          "call *__imp_SetUnhandledExceptionFilter(%rip)\n\t"
          "mov $43, %ecx\n\t"
          "mov __imp_ExitProcess(%rip), %rax\n\t"
          "call *%rax");
}
PROGRAM_CODE __declspec(dllexport) __attribute__((naked, used)) U32
late_program(void) {
  __asm__("sub $40, %rsp\n\t"
          "call *LateImports(%rip)\n\t"
          "mov $43, %ecx\n\t"
          "mov __imp_ExitProcess(%rip), %rax\n\t"
          "call *%rax");
}
PROGRAM_CODE __declspec(dllexport) __attribute__((naked, used)) U32
address_previous_prefix(void) {
  __asm__("push %rbx\n\t"
          "sub $32, %rsp\n\t"
          "mov $0x41000000, %eax\n\t"
          "mov __imp_GetStdHandle(%rip), %rbx\n\t"
          "mov $0xfffffff5, %ecx\n\t"
          "call *%rbx\n\t"
          "mov $43, %ecx\n\t"
          "mov __imp_ExitProcess(%rip), %rax\n\t"
          "call *%rax");
}
#endif

// A call into the program that a loader makes before it leaves, as a system
// loader calls initializers. The volatile cell keeps the call observable to
// the compiler and holds the value it was linked with.
static volatile U32 Initialized = InitializeResult;
PROGRAM_CODE static U32 initialize(void) {
  Initialized = InitializeResult;
  return Initialized;
}

PROGRAM_CODE static U32 status(U32 Written) {
  U32 Sum = 0;
  for (U32 I = 0; I < Written; ++I)
    Sum += (U8)Message[I];
  return Sum ? ExitStatus : FailureStatus;
}

#if defined(__x86_64__)
static volatile U32 NativeDelayNumber;
static volatile U64 NativeDelayInterior;
__attribute__((naked, noinline)) static U32
directDelay(U64 Alert, const long long *Interval, U32 Number) {
  __asm__("movq %rcx, %r10\n\t"
          "movl %r8d, %eax\n\t"
          "syscall\n\t"
          "retq\n\t");
}
__attribute__((naked, noinline)) static U32
interiorDelay(U64 Alert, const long long *Interval, U32 Number, U64 Interior) {
  __asm__("movq %rcx, %r10\n\t"
          "movl %r8d, %eax\n\t"
          "jmpq *%r9\n\t");
}
// Keep the failure exit outside .prog so the ordinary tail-call oracle still
// executes every import call that its independent packer transforms.
__attribute__((noinline)) static void useDirectService(void) {
  const long long Interval = -1;
  const U32 Result =
      Pack.Mode == LateDirectServiceMode
          ? interiorDelay(0, &Interval, NativeDelayNumber, NativeDelayInterior)
          : directDelay(0, &Interval, NativeDelayNumber);
  if (Result != 0)
    ExitProcess(FailureStatus);
}
#endif

PROGRAM_ENTRY U32 program(void) {
  const int RuntimeOK =
      (Pack.Mode != OwnedRuntimeMode || checkRuntimeState()) &&
      (Pack.Mode != VirtualRuntimeMode || checkVirtualState());
  if (Pack.Mode == LateEncodedPointerMode)
    EncodedState = encodeNull(Pack.Mode);
  const int PointerOK = (Pack.Mode != EncodedPointerMode &&
                         Pack.Mode != NativeEncodedPointerMode &&
                         Pack.Mode != LateEncodedPointerMode) ||
                        decodesToNull(Pack.Mode, EncodedState);
#if defined(__x86_64__)
  if (Pack.Mode == DirectServiceMode || Pack.Mode == LateDirectServiceMode)
    useDirectService();
#endif
  // Keep one exit call so ordinary import-repair cases reach every import
  // site that the independent test packer transforms.
  const U32 HeapResult =
      Pack.Mode == HeapStateMode ? *HeapState : InitializeResult;
#if defined(__x86_64__)
  if (Pack.Mode == ReboundOpaqueCallMode || Pack.Mode == LateOpaqueCallMode) {
    resolveLateImport();
    __attribute__((musttail)) return late_program();
  }
  if (Pack.Mode == OpaqueCallMode) {
    __attribute__((musttail)) return opaque_program();
  }
  if (Pack.Mode == PreviousPrefixAddressMode) {
    __attribute__((musttail)) return address_previous_prefix();
  }
  if (Pack.Mode == ExtendedAddressMode || Pack.Mode == CompactAddressMode) {
    __attribute__((musttail)) return address_extended_program();
  }
  if (Pack.Mode == ChangingAddressMode || Pack.Mode == UnresolvedAddressMode) {
    __attribute__((musttail)) return address_twice();
  }
  if (Pack.Mode == AddressMode || Pack.Mode == ImpureAddressMode ||
      Pack.Mode == ServiceAddressMode || Pack.Mode == CallOnlyAddressMode) {
    __attribute__((musttail)) return address_program();
  }
#endif
  U32 Written = 0;
  WriteFile(GetStdHandle(StdoutSelector), Message, sizeof(Message) - 1,
            &Written, 0);
#if defined(__x86_64__)
  // Keep one ordinary exit call in the byte-for-byte oracle. Dropping the
  // impure helper's persistent increment must still change the exit status.
  Written &= 0u - ((Pack.Mode != ImpureCallMode) | (AddressEffects == 1));
#endif
  ExitProcess(HeapResult == InitializeResult && PointerOK && RuntimeOK &&
                      validDynamicState(Pack.Mode)
                  ? status(Written)
                  : FailureStatus);
  return FailureStatus;
}

static void decode(Entry Target, const U8 *In, U32 Bytes) {
  U8 *Out = (U8 *)Target;
  for (U32 I = 0; I < Bytes; ++I)
    Out[I] = (U8)(In[I] ^ Pack.Key);
}

// The second loader is itself code the first loader wrote.
RELAY_ENTRY U32 relay(void) {
  decode(program, Pack.Program, Pack.ProgramBytes);
  __attribute__((musttail)) return program();
}

// Every path leaves by a jump on the stack the process started with, as a
// loader does when it hands control to the program it carried.
__declspec(dllexport) U32 loader(void) {
  prepareDynamicState(Pack.Mode);
  if (Pack.Mode == OwnedRuntimeMode)
    prepareRuntimeState();
  if (Pack.Mode == VirtualRuntimeMode)
    prepareVirtualState();
  if (Pack.Mode == DecodedPointerMode ||
      Pack.Mode == NativeDecodedPointerMode) {
    EncodedState = 0x12345678;
    // The return is deliberately unused. The retained input is still an
    // observed encoding value, not proof that its storage is a live pointer.
    decodesToNull(Pack.Mode, EncodedState);
  }
  if (Pack.Mode == EncodedPointerMode ||
      Pack.Mode == NativeEncodedPointerMode ||
      Pack.Mode == ClearedEncodedPointerMode) {
    EncodedState = encodeNull(Pack.Mode);
    if (Pack.Mode == ClearedEncodedPointerMode)
      EncodedState = 0;
  }
#if defined(__x86_64__)
  if (Pack.Mode == DirectServiceMode || Pack.Mode == LateDirectServiceMode) {
    const U8 *Gate =
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDelayExecution");
    if (!Gate || Gate[0] != 0x4c || Gate[1] != 0x8b || Gate[2] != 0xd1 ||
        Gate[3] != 0xb8)
      ExitProcess(FailureStatus);
    NativeDelayNumber = *(const U32 *)(Gate + 4);
    NativeDelayInterior = (U64)(Gate + 8);
    if (Pack.Mode == DirectServiceMode)
      useDirectService();
  }
#endif
  if (Pack.Mode == HeapStateMode || Pack.Mode == ReleasedHeapStateMode) {
    HeapState = (U32 *)HeapAlloc(GetProcessHeap(), 0, sizeof(*HeapState));
    if (!HeapState)
      ExitProcess(FailureStatus);
    *HeapState = InitializeResult;
    if (Pack.Mode == ReleasedHeapStateMode) {
      if (!HeapFree(GetProcessHeap(), 0, (void *)HeapState))
        ExitProcess(FailureStatus);
      HeapState = 0;
    }
  }
#if defined(__x86_64__)
  if (Pack.Mode == ReboundOpaqueCallMode)
    resolveLateImport();
  if ((Pack.Mode >= AddressMode && Pack.Mode <= CompactAddressMode) ||
      Pack.Mode == CallOnlyAddressMode)
    CachedExport[0] = (U64)GetStdHandle;
#endif
  if (Pack.Mode == StagedMode) {
    decode(relay, Pack.Relay, Pack.RelayBytes);
    __attribute__((musttail)) return relay();
  }
  decode(program, Pack.Program, Pack.ProgramBytes);
  if (Pack.Mode == CallMode && initialize() != InitializeResult)
    ExitProcess(FailureStatus);
  __attribute__((musttail)) return program();
}
