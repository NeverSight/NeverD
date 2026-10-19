// Original PE/PDB reference with relocatable KD layout facts. Ordinary driver
// execution still traps; dedicated initializer probes run only in isolation.
typedef unsigned long long U64;
#ifdef MCFG_REFERENCE
// These file initializers are deliberately not guest runtime observations.
__declspec(dllexport) unsigned int HalpPciMcfgTableCount = 0x13579bdf;
__declspec(dllexport) U64 HalpPciMcfgTable = 0x1122334455667788ULL;
#endif
// Original linker-generated GFIDS, with a minimal read-only PE32+ directory.
#ifdef CFG_METADATA_REFERENCE
__asm__(".section .rdata,\"dr\"\n.p2align 3\n.globl _load_config_used\n"
        "_load_config_used:\n.long 0x94\n.zero 108\n.quad 0\n.quad 0\n"
        ".quad ReferenceGfids\n.quad 1\n.long __guard_flags + 0x1000c000\n"
        "ReferenceGfids:\n.rva KeGetCurrentIrql\n"
#ifdef CFG_FID_SUPPRESSED
        ".byte 1\n"
#else
        ".byte 2\n"
#endif
);
#else
__asm__(".section .rdata,\"dr\"\n.p2align 3\n.globl _load_config_used\n"
        "_load_config_used:\n.long 0x94\n.zero 108\n.quad 0\n.quad 0\n"
        ".quad __guard_fids_table\n.quad __guard_fids_count\n"
        ".long __guard_flags\n");
#endif
__declspec(dllexport) unsigned char KeGetCurrentIrql(void) {
#ifdef OTHER_COMPANION
  return 0x34;
#else
  return 0x33;
#endif
}
__declspec(dllexport) U64 KdpDebuggerDataListHead[2];
__declspec(dllexport) U64 KdpLoaderDebuggerBlock;
__declspec(dllexport) U64 PsLoadedModuleList[2];
__declspec(dllexport) U64 PsActiveProcessHead[2];
// Deliberately invalid native pointers: only the guest model may publish
// values.
__declspec(dllexport) U64 PsThreadType = 0x1122334455667788ULL;
__declspec(dllexport) U64 PsProcessType = 0x2233445566778899ULL;
__declspec(dllexport) U64 IoFileObjectType = 0x33445566778899aaULL;
__declspec(dllexport) U64 IoDriverObjectType = 0x445566778899aabbULL;
// Deliberately different from the real kernel offsets: extraction must use
// this authenticated image, never copy 0x440/0x448 from the host profile.
__declspec(dllexport) U64 PsGetProcessId(const void *Process) {
  return *(const U64 *)((const char *)Process + 0x120);
}
// Deliberately different from native 19041: private thread fields must be
// extracted from this companion's accessors, including the contiguous CID.
__declspec(dllexport) U64 PsGetThreadId(const void *Thread) {
  return *(const U64 *)((const char *)Thread + 0x198);
}
__declspec(dllexport) U64 PsGetThreadProcessId(const void *Thread) {
  return *(const U64 *)((const char *)Thread + 0x190);
}
__declspec(dllexport) U64 PsGetThreadProcess(const void *Thread) {
  return *(const U64 *)((const char *)Thread + 0x1c0);
}
__declspec(dllexport) U64 IoThreadToProcess(const void *Thread) {
  return *(const U64 *)((const char *)Thread + 0x1c0);
}
__declspec(dllexport) U64 PsGetThreadWin32Thread(const void *Thread) {
  return *(const U64 *)((const char *)Thread + 0x1d0);
}
__declspec(dllexport) __attribute__((naked)) void
PsGetCurrentThreadWin32Thread(void) {
  __asm__("movq %gs:0x188,%rax\nmovq 0x1d0(%rax),%rax\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) void PsSetThreadWin32Thread(void) {
  __asm__("xorq %r9,%r9\ntestq %rdx,%rdx\nje 1f\n"
          "xchgq %rdx,0x1d0(%rcx)\nretq\n"
          "1: movq %r8,%rax\nlock cmpxchgq %r9,0x1d0(%rcx)\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) void
PsQueryThreadStartAddress(void) {
  __asm__(".byte 0xf7,0x41,0x74,0,4,0,0,0x4c,0x8b,0xc9\n"
          ".byte 0x75,7,0x45,0x33,0xc0,0x85,0xd2,0x75,9\n"
          ".byte 0x48,0x8b,0x81\n.long 0x1e0\nretq\nint3\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) void PsGetNextProcess(void) {
  // An analysis-only accessor block. Entry is a trap, like other unmodeled
  // reference routines; the loader observes only the authenticated index block.
  __asm__(".fill 62,1,0xcc\n"
          "movq PsActiveProcessHead(%rip), %rbx\n"
          "testq %rdi, %rdi\n.byte 0x74,0x07,0x48,0x8b,0x9f\n.long 0x180\n"
          "leaq PsActiveProcessHead(%rip), %r15\n"
          ".byte 0x49,0x3b,0xdf,0x74,0x1d,0x4c,0x8d,0xb3\n.long -0x180\n"
          "retq\n");
}
__declspec(dllexport) U64 CommitCounter;
// Independent original-code initialization probes. These exercise the private
// initializer, never the driver's ordinary API-dispatch route.
__declspec(dllexport) U64 ReferenceInitCopy(U64 *Output, const U64 *Input) {
  Output[0] = *Input + 3;
  return 0;
}
__declspec(dllexport) U64 ReferenceInitNext(U64 *Output) {
  Output[1] = Output[0] + 7;
  return 0;
}
__declspec(dllexport) __attribute__((noinline, naked)) U64
ReferenceInitResource(U64 *Output, U64 Input) {
  __asm__("int3\nretq\n");
}
__declspec(dllexport) U64 ReferenceInitViaService(U64 *Output, U64 Input) {
  U64 Value = ReferenceInitResource(Output, Input);
  Output[1] = Value + 4;
  return 0;
}
__declspec(dllexport) U64 ReferenceInitUndefined(U64 *Output) {
  Output[0] = *(volatile U64 *)&CommitCounter;
  return 0;
}
__declspec(dllexport) __attribute__((naked)) void ReferenceInitBadReturn(void) {
  __asm__("movq (%rsp),%rax\naddq $16,%rsp\njmpq *%rax\n");
}
__declspec(dllexport) __attribute__((naked)) void ReferenceInitSpin(void) {
  __asm__("1: jmp 1b\n");
}
__declspec(dllexport) U64 ReferenceInitStatus(void) { return 0xc0000001; }
__declspec(dllexport) __attribute__((naked)) void
ReferenceInitPair(U64 *Output, const U64 *Input) {
  __asm__("movdqu (%rdx),%xmm0\nmovdqu %xmm0,(%rcx)\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) void
ReferenceInitAtomic(U64 *Output) {
  __asm__("lock addq $1,(%rcx)\nxorl %eax,%eax\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) void ReferenceInitClock(void) {
  __asm__("rdtsc\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) U64 ReferenceInitIRQL(U64 Ignored,
                                                                   U64 Next) {
  __asm__("stc\nmovq %cr8,%rax\nmovq %rdx,%cr8\n"
          "pushfq\npopq %r8\nmovq %r8,(%rcx)\nretq\n");
}
__declspec(dllexport) U64 SecureCookieSlot;
__declspec(dllexport) __attribute__((naked)) void
ReferenceInitTimestamp(U64 *Output) {
  __asm__("movq $-1,%rax\nmovq $-1,%rdx\nstc\nrdtsc\n"
          "movq %rax,(%rcx)\nmovq %rdx,8(%rcx)\n"
          "pushfq\npopq %rax\nmovq %rax,16(%rcx)\n"
          "movq $-1,%rax\nmovq $-1,%rdx\nrdtsc\n"
          "movq %rax,24(%rcx)\nmovq %rdx,32(%rcx)\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) void
ReferenceInitInterrupts(U64 *Output) {
  __asm__("stc\ncli\npushfq\npopq %rax\nmovq %rax,(%rcx)\n"
          "sti\npushfq\npopq %rax\nmovq %rax,8(%rcx)\nretq\n");
}
#ifdef SERVICE_TABLE_REFERENCE
#pragma section(".sdata", read, write)
__declspec(dllexport)
__declspec(allocate(".sdata")) U64 KeServiceDescriptorTable[4] = {0};
__declspec(dllexport) const unsigned KiServiceLimit = 2;
__declspec(dllexport) const unsigned char KiArgumentTable[2] = {0, 4};
__declspec(dllexport) __attribute__((naked)) unsigned FixtureService0(void) {
  __asm__("movl $0x1234,%eax\nretq\n");
}
__declspec(dllexport) __attribute__((naked)) unsigned FixtureService1(void) {
  __asm__("movl $0x5678,%eax\nretq\n");
}
__asm__(".section .rdata,\"dr\"\n.p2align 2\n.globl KiServiceTable\n"
        ".def KiServiceTable; .scl 2; .type 0; .endef\n"
        "KiServiceTable:\n.rva FixtureService0\n.rva FixtureService1\n.text\n");
__declspec(dllexport) __attribute__((naked)) void KiLockServiceTable(void) {
  __asm__("retq\n");
}
__declspec(dllexport) __attribute__((naked)) void KeCompactServiceTable(void) {
  // Original 19041 compaction instructions, with the call relocated normally.
  __asm__(".byte 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20\n"
          ".byte 0x45,0x8b,0xd0,0x48,0x8b,0xda,0x4c,0x8b,0xd9\n"
          ".byte 0x45,0x85,0xc0,0x74,0x2b,0x41,0x8b,0xfa,0x45,0x8b,0x03\n"
          ".byte 0x0f,0xb6,0x03,0x44,0x2b,0xc1,0x44,0x03,0x44,0x24,0x50\n"
          ".byte 0x48,0xff,0xc3,0x41,0xc1,0xe0,0x04,0xc1,0xe8,0x02\n"
          ".byte 0x44,0x0b,0xc0,0x45,0x89,0x03,0x4d,0x8d,0x5b,0x04\n"
          ".byte 0x48,0x83,0xef,0x01,0x75,0xd8,0x45,0x8b,0xc2\n"
          "callq KiLockServiceTable\n"
          ".byte 0x48,0x8b,0x5c,0x24,0x30,0x48,0x83,0xc4,0x20,0x5f,0xc3\n");
}
__declspec(dllexport) __attribute__((naked)) void KiInitSystem(void) {
  __asm__(".fill 0xe6,1,0xcc\n"
          "leaq KiServiceTable(%rip),%rax\n"
          "movq %rax,KeServiceDescriptorTable(%rip)\n"
          "movl KiServiceLimit(%rip),%eax\n"
          "movaps KeServiceDescriptorTable(%rip),%xmm3\n"
          "movl %eax,KeServiceDescriptorTable+16(%rip)\n"
          "leaq KiArgumentTable(%rip),%rax\n"
          "movq %rax,KeServiceDescriptorTable+24(%rip)\nretq\n");
}
#endif
__declspec(dllexport) __attribute__((naked)) void KiSystemCall64(void) {
  // Only an observed entry prefix. The ordinary guest cannot execute this
  // reference routine, and its GS write is not an initializer instruction.
  __asm__("swapgs\nmovq %rsp,%gs:0x10\n"
#ifdef SERVICE_TABLE_REFERENCE
          "leaq KeServiceDescriptorTable(%rip),%r10\n"
#endif
          "retq\n");
}
__declspec(dllexport) U64 _guard_icall_bitmap;
__declspec(dllexport) __attribute__((naked)) void _guard_dispatch_icall(void) {
  __asm__("movq _guard_icall_bitmap(%rip), %r11\n"
          "testq %rax, %rax\n"
          ".byte 0x0f,0x8d\n.long 1f-.-4\n"
          ".byte 0x4d,0x85,0xdb,0x74\n.byte 1f-.-1\n"
          ".byte 0x4c,0x8b,0xd0,0x49,0xc1,0xea,0x09\n"
          ".byte 0x4f,0x8b,0x1c,0xd3,0x4c,0x8b,0xd0\n"
          ".byte 0x49,0xc1,0xea,0x03,0xa8,0x0f,0x75\n.byte 1f-.-1\n"
          ".byte 0x4d,0x0f,0xa3,0xd3,0x73\n.byte 1f-.-1\n1: retq\n");
}
extern void KernelMisalignedTarget(void);
__declspec(dllexport) void (*volatile KernelMisalignedPointer)(void) =
    KernelMisalignedTarget;
__asm__(".text\n.p2align 4\n.byte 0x90\n.globl KernelMisalignedTarget\n"
        "KernelMisalignedTarget:\nretq\n");
__declspec(dllexport) __attribute__((naked)) void
MmUnsecureVirtualMemory(void *Handle) {
  __asm__("movq %rbx, 0x10(%rsp)\n"
          "pushq %rdi\n"
          "subq $0x20, %rsp\n"
          "movq %gs:0x188, %rax\n"
          "leaq 0x30(%rsp), %rdx\n"
          "movq 0xb8(%rax), %rbx\n"
          "xorq SecureCookieSlot(%rip), %rbx\n"
          ".byte 0x48,0x33,0xd9\n"
          "movq 0x38(%rsp), %rbx\n"
          "addq $0x20, %rsp\n"
          "popq %rdi\n"
          "retq\n");
}
__declspec(dllexport) U64 SystemVaAssignments[28];
__declspec(dllexport) __attribute__((naked)) U64 MiQuerySystemBase(int Type) {
  __asm__(".byte 0x48,0x63,0xc1\n"
          "leaq SystemVaAssignments(%rip), %rcx\n"
          ".byte 0x48,0x03,0xc0,0x48,0x8b,0x04,0xc1,0xc3\n");
}
__declspec(dllexport) void *MmGetPagedPoolCommitPointer(void) {
  return &CommitCounter;
}
#ifdef CODE_PLACED_DEBUGGER
#pragma section(".text", read, execute)
__declspec(allocate(".text"))
#endif
__declspec(dllexport) U64 KdDebuggerDataBlock[0x380 / 8] = {
    [0x20 / 8] = (U64)&KeGetCurrentIrql,
    [0x48 / 8] = (U64)&PsLoadedModuleList,
    [0x138 / 8] = 4096,
    [0x2a8 / 8] = 0xa40,
    [0x2c0 / 8] = 0x898,
    [0x360 / 8] = 0xfffff68000000000ULL};
int DriverEntry(void *Driver, void *Registry) { return 0; }
