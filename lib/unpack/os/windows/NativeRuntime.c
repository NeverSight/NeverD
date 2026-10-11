//===- NativeRuntime.c - Fixed-address Windows object materialization -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
#define IMPORT __declspec(dllimport)
IMPORT void *VirtualAlloc(void *, U64, U32, U32);
IMPORT int VirtualFree(void *, U64, U32);
IMPORT int VirtualProtect(void *, U64, U32, U32 *);
IMPORT int FlushInstructionCache(void *, const void *, U64);
IMPORT void *GetModuleHandleA(const char *);
IMPORT void *GetProcAddress(void *, const char *);
IMPORT void *GetProcessHeap(void);
IMPORT void *HeapCreate(U32, U64, U64);
IMPORT int HeapDestroy(void *);
IMPORT void *HeapAlloc(void *, U32, U64);
IMPORT void *HeapReAlloc(void *, U32, void *, U64);
IMPORT int HeapFree(void *, U32, void *);
IMPORT U64 HeapSize(void *, U32, const void *);
IMPORT int HeapSetInformation(void *, U32, const void *, U64);
IMPORT int FreeEnvironmentStringsW(void *);
IMPORT U32 FlsAlloc(void (*)(void *));
IMPORT int FlsFree(U32);
IMPORT void *FlsGetValue(U32);
IMPORT int FlsSetValue(U32, void *);
IMPORT void InitializeCriticalSection(void *);
IMPORT void EnterCriticalSection(void *);
IMPORT U32 GetLastError(void);
IMPORT void SetLastError(U32);
IMPORT void *GetStdHandle(U32);
IMPORT int WriteFile(void *, const void *, U32, U32 *, void *);
IMPORT void ExitProcess(U32);
typedef struct {
  U64 Address, Size, MappedSize, Heap, Offset;
  U32 Environment, Active;
} Block;
typedef struct {
  U64 Address;
  const char *Module, *Name;
  U64 Actual;
} Gate;
typedef struct {
  U64 Original, Actual;
  const char *Name;
} Provider;
typedef struct {
  U64 Original, Actual;
} Heap;
typedef struct {
  U32 Index, Active;
  U64 Callback, Value;
  U32 Actual, Cleaning;
} Fiber;
typedef struct {
  U64 Address;
  U32 Depth;
} Critical;
typedef struct {
  U64 Address, Size;
} Region;
typedef struct {
  U64 Address, Value;
} Slot;
typedef struct {
  U64 Address, Size;
  U32 Protection, Active;
} Virtual;
typedef struct {
  U64 Address, Size, Offset;
  U32 Protection;
} Page;
#include "payload.h"

static void die(U32 Site) {
  // A stable nonzero process status survives EXE and DLL initializer failure.
  ExitProcess(0xe04e0000u | Site);
}
static int same(const char *A, const char *B) {
  while (*A && *A == *B) {
    ++A;
    ++B;
  }
  return *A == *B;
}
static void copy(U8 *D, const U8 *S, U64 N) {
  for (U64 I = 0; I < N; ++I)
    ((volatile U8 *)D)[I] = S[I];
}
static void zero(U8 *D, U64 N) {
  for (U64 I = 0; I < N; ++I)
    ((volatile U8 *)D)[I] = 0;
}
static void *heap(void *Value) {
  for (U32 I = 0; I < HEAP_COUNT; ++I)
    if ((U64)Value == Heaps[I].Original)
      return (void *)Heaps[I].Actual;
  return Value;
}
static Block *block(const void *Value) {
  for (U32 I = 0; I < BLOCK_COUNT; ++I)
    if (Blocks[I].Active && (U64)Value == Blocks[I].Address)
      return &Blocks[I];
  return 0;
}
static int owner(Block *B, void *H) {
  return !B->Environment && heap((void *)B->Heap) == heap(H);
}
static void *shim_GetProcessHeap(void) { return (void *)PROCESS_HEAP; }
static void *shim_HeapAlloc(void *H, U32 Flags, U64 Size) {
  return HeapAlloc(heap(H), Flags, Size);
}
static int shim_HeapFree(void *H, U32 Flags, void *P) {
  Block *B = block(P);
  if (!B)
    return HeapFree(heap(H), Flags, P);
  if (!owner(B, H)) {
    SetLastError(87);
    return 0;
  }
  if (Flags & ~1u)
    die(20);
  if (!VirtualFree(P, B->MappedSize, 0x4000))
    return 0;
  B->Active = 0;
  return 1;
}
static U64 shim_HeapSize(void *H, U32 Flags, const void *P) {
  Block *B = block(P);
  if (!B)
    return HeapSize(heap(H), Flags, P);
  if (!owner(B, H)) {
    SetLastError(87);
    return (U64)-1;
  }
  return B->Size;
}
static void *shim_HeapReAlloc(void *H, U32 Flags, void *P, U64 Size) {
  Block *B = block(P);
  if (!B)
    return HeapReAlloc(heap(H), Flags, P, Size);
  if (!owner(B, H)) {
    SetLastError(87);
    return 0;
  }
  if (Flags & ~(1u | 8u | 16u))
    die(21);
  U64 Capacity = B->MappedSize;
  if (Size <= Capacity) {
    if (Size > B->Size && (Flags & 8))
      zero((U8 *)P + B->Size, Size - B->Size);
    B->Size = Size;
    return P;
  }
  if (Flags & 16) {
    SetLastError(8);
    return 0;
  }
  void *Q = HeapAlloc(heap(H), Flags & ~16u, Size);
  if (!Q)
    return 0;
  copy(Q, P, B->Size);
  if (!shim_HeapFree(H, Flags & 1u, P))
    die(22);
  return Q;
}
static int shim_HeapDestroy(void *H) {
  if ((U64)H == PROCESS_HEAP)
    die(23);
  for (U32 I = 0; I < BLOCK_COUNT; ++I)
    if (Blocks[I].Active && owner(&Blocks[I], H))
      if (!shim_HeapFree(H, 0, (void *)Blocks[I].Address))
        die(24);
  void *Native = heap(H);
  int Result = HeapDestroy(Native);
  if (Result)
    for (U32 I = 0; I < HEAP_COUNT; ++I)
      if (Heaps[I].Actual == (U64)Native)
        Heaps[I].Actual = 0;
  return Result;
}
static int shim_HeapSetInformation(void *H, U32 Kind, const void *P, U64 N) {
  return HeapSetInformation(heap(H), Kind, P, N);
}
static int shim_FreeEnvironmentStringsW(void *P) {
  Block *B = block(P);
  if (!B)
    return FreeEnvironmentStringsW(P);
  if (!B->Environment) {
    SetLastError(87);
    return 0;
  }
  if (!VirtualFree(P, B->MappedSize, 0x4000))
    return 0;
  B->Active = 0;
  return 1;
}
static U64 shim_Codec(U64 Value) { return Value ^ POINTER_COOKIE; }
static void released(void *Address) {
  for (U32 I = 0; I < VIRTUAL_COUNT; ++I)
    if (Virtuals[I].Address == (U64)Address)
      Virtuals[I].Active = 0;
}
static int shim_VirtualFree(void *Address, U64 Size, U32 Type) {
  int Result = VirtualFree(Address, Size, Type);
  if (Result && Type == 0x8000)
    released(Address);
  return Result;
}
static U32 shim_NtFreeVirtualMemory(void *Process, void **Address, U64 *Size,
                                    U32 Type) {
  U32 SavedError = GetLastError();
  U32 (*Free)(void *, void **, U64 *, U32) =
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtFreeVirtualMemory");
  if (!Free)
    die(26);
  void *Old = *Address;
  SetLastError(SavedError);
  U32 Status = Free(Process, Address, Size, Type);
  if (!(Status & 0x80000000u) && Process == (void *)-1 && Type == 0x8000)
    released(Old);
  return Status;
}
static Fiber *fiber(U32 Index) {
  if (Index >= FIBER_CAPACITY || !Fibers[Index].Active)
    return 0;
  return &Fibers[Index];
}
static U32 shim_FlsAlloc(void (*Callback)(void *)) {
  for (U32 I = 0; I < FIBER_CAPACITY; ++I) {
    if (Fibers[I].Active)
      continue;
    U32 Actual = FlsAlloc(Callback);
    if (Actual == (U32)-1)
      return Actual;
    Fibers[I].Active = 1;
    Fibers[I].Actual = Actual;
    Fibers[I].Callback = (U64)Callback;
    return I;
  }
  SetLastError(8);
  return (U32)-1;
}
static int shim_FlsFree(U32 Index) {
  Fiber *F = fiber(Index);
  if (!F) {
    SetLastError(87);
    return 0;
  }
  if (F->Cleaning)
    die(25);
  F->Cleaning = 1;
  int Result = FlsFree(F->Actual);
  F->Cleaning = 0;
  if (Result)
    F->Active = 0;
  return Result;
}
static void *shim_FlsGetValue(U32 Index) {
  Fiber *F = fiber(Index);
  if (!F) {
    SetLastError(87);
    return 0;
  }
  return FlsGetValue(F->Actual);
}
static int shim_FlsSetValue(U32 Index, void *Value) {
  Fiber *F = fiber(Index);
  if (!F) {
    SetLastError(87);
    return 0;
  }
  return FlsSetValue(F->Actual, Value);
}
static void *shim_GetProcAddress(void *Module, const char *Name) {
  for (U32 I = 0; I < PROVIDER_COUNT; ++I)
    if ((U64)Module == Providers[I].Original)
      Module = (void *)Providers[I].Actual;
  void *Value = GetProcAddress(Module, Name);
  if (!Value)
    return Value;
  for (U32 I = 0; I < GATE_COUNT; ++I)
    if (Gates[I].Actual == (U64)Value &&
        ((U64)Name <= 65535 || same(Gates[I].Name, Name)))
      return (void *)Gates[I].Address;
  return Value;
}
static void missing(void) { die(30); }
static void *binding(Gate *G) {
  const char *Name = G->Name;
  int Kernel =
      same(G->Module, "kernel32.dll") || same(G->Module, "kernelbase.dll");
  int Native = same(G->Module, "ntdll.dll");
#define BIND(Symbol)                                                           \
  if (Kernel && same(Name, #Symbol))                                           \
  return (void *)&shim_##Symbol
  BIND(GetProcessHeap);
  BIND(HeapAlloc);
  BIND(HeapFree);
  BIND(HeapReAlloc);
  BIND(HeapSize);
  BIND(HeapDestroy);
  BIND(HeapSetInformation);
  BIND(FreeEnvironmentStringsW);
  BIND(FlsAlloc);
  BIND(FlsFree);
  BIND(FlsGetValue);
  BIND(FlsSetValue);
  BIND(GetProcAddress);
  BIND(VirtualFree);
#undef BIND
  if (Native && same(Name, "RtlAllocateHeap"))
    return (void *)&shim_HeapAlloc;
  if (Native && same(Name, "RtlFreeHeap"))
    return (void *)&shim_HeapFree;
  if (Native && same(Name, "RtlReAllocateHeap"))
    return (void *)&shim_HeapReAlloc;
  if (Native && same(Name, "RtlSizeHeap"))
    return (void *)&shim_HeapSize;
  if (Native &&
      (same(Name, "NtFreeVirtualMemory") || same(Name, "ZwFreeVirtualMemory")))
    return (void *)&shim_NtFreeVirtualMemory;
  if ((Kernel &&
       (same(Name, "EncodePointer") || same(Name, "DecodePointer"))) ||
      (Native &&
       (same(Name, "RtlEncodePointer") || same(Name, "RtlDecodePointer"))))
    return (void *)&shim_Codec;
  return G->Actual ? (void *)G->Actual : (void *)&missing;
}
static U32 Initialized;
__declspec(dllexport) void restore(void *Image, U32 Reason, void *Reserved) {
  (void)Reserved;
  if (Reason != 1 || Initialized)
    return;
  if ((U64)Image != IMAGE_BASE)
    die(1);
  U8 *PEB;
  __asm__ volatile("movq %%gs:0x60,%0" : "=r"(PEB));
  if (*(U32 *)(PEB + 0x118) != VERSION_MAJOR ||
      *(U32 *)(PEB + 0x11c) != VERSION_MINOR ||
      *(U16 *)(PEB + 0x120) != VERSION_BUILD ||
      *(U32 *)(PEB + 0x124) != VERSION_PLATFORM)
    die(2);
  for (U32 I = 0; I < VIRTUAL_COUNT; ++I)
    if (VirtualAlloc((void *)Virtuals[I].Address, Virtuals[I].Size, 0x2000,
                     Virtuals[I].Protection) != (void *)Virtuals[I].Address)
      die(13);
  for (U32 I = 0; I < PAGE_COUNT; ++I) {
    Page *P = &Pages[I];
    if (VirtualAlloc((void *)P->Address, P->Size, 0x1000, 4) !=
        (void *)P->Address)
      die(14);
    copy((void *)P->Address, Payload + P->Offset, P->Size);
    U32 Old;
    if (!VirtualProtect((void *)P->Address, P->Size, P->Protection, &Old))
      die(15);
  }
  for (U32 I = 0; I < REGION_COUNT; ++I)
    if (VirtualAlloc((void *)Regions[I].Address, Regions[I].Size, 0x2000, 1) !=
        (void *)Regions[I].Address)
      die(3);
  for (U32 I = 0; I < BLOCK_COUNT; ++I) {
    Block *B = &Blocks[I];
    if (VirtualAlloc((void *)B->Address, B->MappedSize, 0x1000, 4) !=
        (void *)B->Address)
      die(11);
    copy((void *)B->Address, Payload + B->Offset, B->MappedSize);
  }
  for (U32 I = 0; I < HEAP_COUNT; ++I) {
    Heaps[I].Actual =
        (U64)(Heaps[I].Original == PROCESS_HEAP ? GetProcessHeap()
                                                : HeapCreate(0, 0, 0));
    if (!Heaps[I].Actual)
      die(4);
  }
  for (U32 I = 0; I < PROVIDER_COUNT; ++I) {
    void *M = GetModuleHandleA(Providers[I].Name);
    Providers[I].Actual = (U64)M;
    if (!M)
      die(5);
  }
  for (U32 I = 0; I < GATE_REGION_COUNT; ++I)
    if (VirtualAlloc((void *)GateRegions[I].Address, GateRegions[I].Size,
                     0x3000, 0x40) != (void *)GateRegions[I].Address)
      die(6);
  for (U32 I = 0; I < GATE_COUNT; ++I) {
    Gate *G = &Gates[I];
    void *M = GetModuleHandleA(G->Module);
    G->Actual = (U64)(M ? GetProcAddress(M, G->Name) : 0);
    U8 *At = (U8 *)G->Address;
    U64 Target = (U64)binding(G);
    At[0] = 0xff;
    At[1] = 0x25;
    zero(At + 2, 4);
    copy(At + 6, (U8 *)&Target, 8);
  }
  for (U32 I = 0; I < SLOT_COUNT; ++I) {
    U32 Old, Ignored;
    if (!VirtualProtect((void *)Slots[I].Address, 8, 4, &Old))
      die(7);
    copy((void *)Slots[I].Address, (U8 *)&Slots[I].Value, 8);
    if (!VirtualProtect((void *)Slots[I].Address, 8, Old, &Ignored))
      die(8);
  }
  for (U32 I = 0; I < CRITICAL_COUNT; ++I) {
    InitializeCriticalSection((void *)Criticals[I].Address);
    for (U32 J = 0; J < Criticals[I].Depth; ++J)
      EnterCriticalSection((void *)Criticals[I].Address);
  }
  for (U32 I = 0; I < FIBER_CAPACITY; ++I) {
    Fiber *F = &Fibers[I];
    if (!F->Active)
      continue;
    F->Actual = FlsAlloc((void (*)(void *))F->Callback);
    if (F->Actual == (U32)-1 || !FlsSetValue(F->Actual, (void *)F->Value))
      die(9);
  }
  for (U32 I = 0; I < GATE_REGION_COUNT; ++I) {
    U32 Old;
    if (!VirtualProtect((void *)GateRegions[I].Address, GateRegions[I].Size,
                        0x20, &Old))
      die(12);
  }
  if (!FlushInstructionCache((void *)-1, 0, 0))
    die(10);
  Initialized = 1;
  SetLastError(LAST_ERROR);
}
__declspec(dllexport) int library_entry(void *Image, U32 Reason,
                                        void *Reserved) {
  int (*Original)(void *, U32, void *) = (void *)LOADER_ENTRY;
  if (Reason == 1)
    SetLastError(LAST_ERROR);
  int Result = Original(Image, Reason, Reserved);
  if (Reason == 0 || (Reason == 1 && !Result)) {
    for (U32 I = 0; I < FIBER_CAPACITY; ++I)
      if (Fibers[I].Active)
        die(40);
    for (U32 I = 0; I < REGION_COUNT; ++I)
      if (!VirtualFree((void *)Regions[I].Address, 0, 0x8000))
        die(41);
    for (U32 I = 0; I < VIRTUAL_COUNT; ++I)
      if (Virtuals[I].Active &&
          !VirtualFree((void *)Virtuals[I].Address, 0, 0x8000))
        die(44);
    for (U32 I = 0; I < GATE_REGION_COUNT; ++I)
      if (!VirtualFree((void *)GateRegions[I].Address, 0, 0x8000))
        die(42);
    for (U32 I = 0; I < HEAP_COUNT; ++I)
      if (Heaps[I].Original != PROCESS_HEAP && Heaps[I].Actual)
        if (!HeapDestroy((void *)Heaps[I].Actual))
          die(43);
  }
  return Result;
}

// The loader can change LastError after TLS initialization. Restore it at the
// actual EXE handoff without changing any entry registers, flags or stack.
__attribute__((used)) static const U64 ResumeEntry = LOADER_ENTRY;
__declspec(dllexport) __attribute__((naked)) void program_entry(void) {
  __asm__("movl %0, %%gs:0x68\n\t"
          "jmp *ResumeEntry(%%rip)"
          :
          : "i"(LAST_ERROR));
}
