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

#pragma section(".pack", read, write)
struct {
  U32 Mode, Key, Size, Reserved;
  U8 Bytes[65536];
} __declspec(allocate(".pack")) Packed = {0, 0xa5, 0, 0, {0}};
static void *volatile RetainedPool;
static const UNICODE_STRING *volatile RetainedPath;
static UNICODE_STRING RoutineName;
static void *volatile Resolved[2];
__attribute__((used)) static const U32 ChangedMXCSR = 0x3f80;

__attribute__((noinline, used)) static void unpack_bytes(DRIVER_OBJECT *Driver,
                                                         UNICODE_STRING *Path) {
  for (U32 I = 0; I < Packed.Size; ++I)
    ((volatile U8 *)ProgramStart)[I] = Packed.Bytes[I] ^ (U8)Packed.Key;
  if (Packed.Mode == 1)
    RetainedPool = ExAllocatePoolWithTag(0, 32, 0x44564e55);
  else if (Packed.Mode == 2)
    Driver->MajorFunction[0] = (void *)DriverEntry;
  else if (Packed.Mode == 3)
    RetainedPath = Path;
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
