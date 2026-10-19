// Original fixture: fixed guest CR4/CET reads, widths and flags.
typedef unsigned long long U64;
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  U64 Before, After, Control;
  __asm__ volatile("pushfq; popq %0; movq %%cr4, %%r8; movq %%r8, %2; "
                   "pushfq; popq %1"
                   : "=&r"(Before), "=&r"(After), "=&r"(Control)
                   :
                   : "r8", "memory");
  if (Control != 0x620 || Before != After)
    return (int)0xc0ffee01;
#ifdef WRITE_CR4
  __asm__ volatile("movq %0, %%cr4" : : "r"(Control) : "memory");
#endif
  U64 A = ~0ULL, D = ~0ULL, C = 0xaaaaaaaa000006a2ULL;
  __asm__ volatile("pushfq; popq %3; rdmsr; pushfq; popq %4"
                   : "+a"(A), "+d"(D), "+c"(C), "=&r"(Before), "=&r"(After)
                   :
                   : "memory");
  if (A || D || C != 0xaaaaaaaa000006a2ULL || Before != After)
    return (int)0xc0ffee02;
  return 0;
}
