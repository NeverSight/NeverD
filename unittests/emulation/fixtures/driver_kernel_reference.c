//===- driver_kernel_reference.c - Original reference-image fixture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllexport) unsigned char KeGetCurrentIrql(void) {
  return 0x33; // Deliberately different from the modeled PASSIVE_LEVEL.
}
__declspec(dllexport) unsigned long long ReferenceAccessor(void *Object) {
  return *(unsigned long long *)((unsigned char *)Object + 0x80);
}
__declspec(dllexport) volatile unsigned ReferenceData = 0xaabbccdd;
// The byte before each real entry is exported only to locate an interior
// instruction without relying on a fixed PE address or a modeled API name.
#ifdef REFERENCE_THREAD_TRANSFERS
__declspec(dllexport)
__attribute__((naked)) void ReferenceThreadTransfer(void) {
  __asm__("int3; jmp *%rcx; nop; jmp *%rcx");
}
__declspec(dllexport) __attribute__((naked)) void
ReferenceThreadNonTransfer(void) {
  __asm__("int3; movq %rcx, %rax; jmp *%rcx");
}
__declspec(dllexport) __attribute__((naked)) void
ReferenceThreadMemoryTransfer(void) {
  __asm__("int3; jmp *(%rcx)");
}
#endif
int DriverEntry(void *Driver, void *Registry) { return 0; }
