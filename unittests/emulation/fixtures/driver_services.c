// Follow a real RDMSR and RIP-relative reference to observe runtime services.
typedef unsigned long long U64;
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  unsigned Low, High;
  __asm__ volatile("rdmsr" : "=a"(Low), "=d"(High) : "c"(0xc0000082));
  const volatile unsigned char *Entry =
      (const volatile unsigned char *)(((U64)High << 32) | Low);
  if (Entry[12] != 0x4c || Entry[13] != 0x8d || Entry[14] != 0x15)
    return (int)0xc000000d;
  volatile U64 *Descriptor =
      (volatile U64 *)(Entry + 19 + *(const int *)(Entry + 15));
#ifdef WRITE_DESCRIPTOR
  Descriptor[0] = 0;
#endif
  if (Descriptor[1] || Descriptor[2] != 2)
    return (int)0xc000000d;
  const volatile int *Table = (const volatile int *)Descriptor[0];
  const volatile unsigned char *Arguments =
      (const volatile unsigned char *)Descriptor[3];
#ifdef WRITE_TABLE
  *(volatile int *)Table = 0;
#endif
  for (unsigned I = 0; I < 2; ++I) {
    int Encoded = Table[I];
    const volatile unsigned char *Target =
        (const volatile unsigned char *)Table + (Encoded >> 4);
    if ((Encoded & 15) != I || Arguments[I] != I * 4 || Target[0] != 0xb8 ||
        *(const volatile unsigned *)(Target + 1) != (I ? 0x5678 : 0x1234) ||
        Target[5] != 0xc3)
      return (int)0xc000000d;
  }
  return 0;
}
