// Original CPUID client: check the complete instruction result at full width.
typedef unsigned long long U64;
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  U64 Output[7];
  __asm__ volatile("movabsq $0xaaaaaaaa40000000, %%rax\n"
                   "movabsq $0xbbbbbbbb00000003, %%rcx\n"
                   "movq $-1, %%rbx\n"
                   "movq $-1, %%rdx\n"
                   "movabsq $0x123456789abcdef0, %%r8\n"
                   "xorl %%r9d, %%r9d\n"
                   "stc\n"
                   "pushfq\n"
                   "popq %%r9\n"
                   "cpuid\n"
                   "pushfq\n"
                   "popq %%r10\n"
                   "movq %%rax, 0(%0)\n"
                   "movq %%rbx, 8(%0)\n"
                   "movq %%rcx, 16(%0)\n"
                   "movq %%rdx, 24(%0)\n"
                   "movq %%r8, 32(%0)\n"
                   "movq %%r9, 40(%0)\n"
                   "movq %%r10, 48(%0)\n"
                   :
                   : "r"(Output)
                   : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10", "cc",
                     "memory");
  if (Output[0] != 0xfedcba98 || Output[1] != 0x80000001 ||
      Output[2] != 0x01234567 || Output[3] != 0xffffffff ||
      Output[4] != 0x123456789abcdef0 || Output[5] != Output[6] ||
      (Output[5] & 0x45) != 0x45)
    return (int)0xc000000d;
  return 0;
}
