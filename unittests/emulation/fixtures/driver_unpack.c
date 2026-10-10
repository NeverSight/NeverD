//===- driver_unpack.c - Independently linked packed driver fixture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// The payload uses the existing WDM lifecycle oracle. Only this fixture's
// private pack record knows how its bytes are transformed.
#pragma code_seg(push, ".prog$a")
__attribute__((naked, used)) void ProgramStart(void) {
  __asm__ volatile(".rept 16\n\tint3\n\t.endr");
}
#pragma code_seg(pop)
#pragma code_seg(push, ".prog$m")
#include "driver_io.c"
#pragma code_seg(pop)

__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const UNICODE_STRING *);
__declspec(dllimport) void *IoAllocateMdl(void *, U32, U8, U8, void *);
__declspec(dllimport) void MmProbeAndLockPages(void *, U8, U32);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(void *, U8, U32,
                                                         void *, U32, U32);
__declspec(dllimport) void MmUnmapLockedPages(void *, void *);
__declspec(dllimport) void MmUnlockPages(void *);
__declspec(dllimport) void IoFreeMdl(void *);
__declspec(dllimport) void RtlCopyMemory(void *, const void *, U64);
__declspec(dllimport) U64 KeQueryActiveProcessors(void);
__declspec(dllimport) void KeSetSystemAffinityThread(U64);
__declspec(dllimport) void KeRevertToUserAffinityThread(void);
struct ImageMDL {
  void *Next;
  unsigned short Size, Flags;
  void *Process, *MappedSystemVA, *StartVA;
  U32 ByteCount, ByteOffset;
};

#pragma section(".pack", read, write)
struct {
  U32 Mode, Key, Size, Reserved;
  U8 Bytes[65536];
} __declspec(allocate(".pack")) Packed = {0, 0xa5, 0, 0, {0}};
static void *volatile RetainedPool;
static const UNICODE_STRING *volatile RetainedPath;
static UNICODE_STRING RoutineName;
static void *volatile Resolved[2];
static volatile U64 ObservedPhysicalPage;
static volatile U64 ObservedCounter, ObservedFrequency;
static volatile U32 ObservedCPUID[4];
__attribute__((used)) static const U32 ChangedMXCSR = 0x3f80;

__attribute__((noinline, used)) static void unpack_bytes(DRIVER_OBJECT *Driver,
                                                         UNICODE_STRING *Path) {
  volatile U8 *Destination = (volatile U8 *)ProgramStart;
  void *MDL = 0, *Mapping = 0;
  if (Packed.Mode >= 11 && Packed.Mode <= 14) {
    MDL = IoAllocateMdl((void *)ProgramStart, Packed.Size, 0, 0, 0);
    MmProbeAndLockPages(MDL, 0, 1);
    Mapping = MmMapLockedPagesSpecifyCache(MDL, 0, 1, 0, 0, 0x40000010U);
    Destination = (volatile U8 *)Mapping;
    if (Packed.Mode == 13)
      ObservedPhysicalPage =
          *(volatile U64 *)((U8 *)MDL + sizeof(struct ImageMDL));
    else if (Packed.Mode == 14)
      RtlCopyMemory((void *)&ObservedPhysicalPage,
                    (U8 *)MDL + sizeof(struct ImageMDL), sizeof(U64));
  }
  for (U32 I = 0; I < Packed.Size; ++I)
    Destination[I] = Packed.Bytes[I] ^ (U8)Packed.Key;
  if (MDL && Packed.Mode != 12) {
    MmUnmapLockedPages(Mapping, MDL);
    MmUnlockPages(MDL);
    IoFreeMdl(MDL);
  }
  if (Packed.Mode == 15) {
    const U64 Mask = KeQueryActiveProcessors();
    if (Mask != 1)
      __builtin_trap();
    KeSetSystemAffinityThread(Mask);
    KeRevertToUserAffinityThread();
  }
  if (Packed.Mode == 16) {
    typedef U64 (*CounterFunction)(U64 *);
    RtlInitUnicodeString(&RoutineName, L"KeQueryPerformanceCounter");
    const CounterFunction Counter =
        (CounterFunction)MmGetSystemRoutineAddress(&RoutineName);
    ObservedCounter = Counter((U64 *)&ObservedFrequency);
  }
  if (Packed.Mode == 17 || Packed.Mode == 18) {
    U32 Low, High;
    if (Packed.Mode == 17)
      __asm__ volatile("rdtsc" : "=a"(Low), "=d"(High));
    else
      __asm__ volatile("rdtscp" : "=a"(Low), "=d"(High) : : "rcx");
    ObservedCounter = ((U64)High << 32) | Low;
  }
  if (Packed.Mode == 19) {
    U32 A = 0, B, C = 0, D;
    __asm__ volatile("cpuid" : "+a"(A), "=b"(B), "+c"(C), "=d"(D));
    ObservedCPUID[0] = A;
    ObservedCPUID[1] = B;
    ObservedCPUID[2] = C;
    ObservedCPUID[3] = D;
  }
  if (Packed.Mode == 20) {
    RetainedPool = ExAllocatePoolWithTag(0, 32, 0x44564e55);
    Driver->MajorFunction[0] = (void *)DriverEntry;
  } else if (Packed.Mode == 1)
    RetainedPool = ExAllocatePoolWithTag(0, 32, 0x44564e55);
  else if (Packed.Mode == 2)
    Driver->MajorFunction[0] = (void *)DriverEntry;
  else if (Packed.Mode == 3)
    RetainedPath = Path;
  else if (Packed.Mode == 10)
    RetainedPool = (void *)((U64)IofCompleteRequest + 1);
  else if (Packed.Mode == 4) {
    RtlInitUnicodeString(&RoutineName, L"IofCompleteRequest");
    Resolved[0] = MmGetSystemRoutineAddress(&RoutineName);
    Resolved[1] = 0;
  }
}

__declspec(dllexport) __attribute__((naked)) NTSTATUS
packed_entry(DRIVER_OBJECT *Driver, UNICODE_STRING *Path) {
  __asm__ volatile("subq $56, %rsp\n\t"
                   "movq %rcx, 32(%rsp)\n\t"
                   "movq %rdx, 40(%rsp)\n\t"
                   "callq unpack_bytes\n\t"
                   "movq 32(%rsp), %rcx\n\t"
                   "movq 40(%rsp), %rdx\n\t"
                   "addq $56, %rsp\n\t"
                   "cmpl $5, Packed(%rip)\n\t"
                   "jne 1f\n\t"
                   "xorq %rcx, %rcx\n\t"
                   "1: cmpl $6, Packed(%rip)\n\t"
                   "jne 2f\n\t"
                   "movq $77, 8(%rsp)\n\t"
                   "2: cmpl $7, Packed(%rip)\n\t"
                   "jne 3f\n\t"
                   "movq $77, %rbx\n\t"
                   "3: cmpl $8, Packed(%rip)\n\t"
                   "jne 4f\n\t"
                   "std\n\t"
                   "4: cmpl $9, Packed(%rip)\n\t"
                   "jne 5f\n\t"
                   "ldmxcsr ChangedMXCSR(%rip)\n\t"
                   "5: jmp DriverEntry");
}
