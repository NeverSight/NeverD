typedef unsigned int U32;
typedef unsigned long long U64;
static void query(U32 Leaf, U32 Subleaf, U32 R[4]) {
  __asm__ volatile("cpuid"
                   : "=a"(R[0]), "=b"(R[1]), "=c"(R[2]), "=d"(R[3])
                   : "a"(Leaf), "c"(Subleaf));
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 R[4];
#ifdef UNOBSERVED_VALID_LEAF
  query(6, 0, R);
  return 0;
#endif
  query(0x40000000, 0, R);
  if (R[0] != 0x40000001 || R[1] != 0x6576654e || R[2] != 0x45204472 ||
      R[3] != 0x2020756d)
    return (int)0xc000000d;
  query(0x40000001, 11, R);
  if (R[0] != 0x3123444e || R[1] || R[2] || R[3])
    return (int)0xc000000d;
  query(0x400000ff, 0xffffffff, R);
  if (R[0] || R[1] || R[2] || R[3])
    return (int)0xc000000d;
  query(8, 0, R);
  if (R[0] != 2 || R[1] != 0x76543210 || R[2] != 0x89abcdef ||
      R[3] != 0xf00d0001)
    return (int)0xc000000d;
  query(0x80000009, 2, R);
  if (R[0] != 0xa1b2c3d4 || R[1] != 0x11223344 || R[2] != 0x55667788 ||
      R[3] != 0x99aabbcc)
    return (int)0xc000000d;
  U64 Output[7];
  __asm__ volatile("movabsq $0xaaaaaaaaaabbccdd, %%rax\n"
                   "movabsq $0xbbbbbbbb00000002, %%rcx\n"
                   "movq $-1, %%rbx\nmovq $-1, %%rdx\n"
                   "movabsq $0x123456789abcdef0, %%r8\n"
                   "xorl %%r9d, %%r9d\nstc\npushfq\npopq %%r9\ncpuid\n"
                   "pushfq\npopq %%r10\n"
                   "movq %%rax, 0(%0)\nmovq %%rbx, 8(%0)\n"
                   "movq %%rcx, 16(%0)\nmovq %%rdx, 24(%0)\n"
                   "movq %%r8, 32(%0)\nmovq %%r9, 40(%0)\nmovq %%r10, 48(%0)\n"
                   :
                   : "r"(Output)
                   : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10", "cc",
                     "memory");
  if (Output[0] != 0xa1b2c3d4 || Output[1] != 0x11223344 ||
      Output[2] != 0x55667788 || Output[3] != 0x99aabbcc ||
      Output[4] != 0x123456789abcdef0 || Output[5] != Output[6] ||
      (Output[5] & 0x45) != 0x45)
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
