// Original instruction client: RDMSR writes zero-extended EDX:EAX, reads only
// ECX, and preserves RCX and flags. The entry code is observed, never called.
typedef unsigned long long U64;
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  U64 Low = ~0ULL, High = ~0ULL, Index = 0x12345678c0000082ULL, Flags;
#ifdef UNKNOWN_MSR
  Index = 0xc0000081;
#endif
#ifdef WRITE_MSR
  __asm__ volatile("stc\nwrmsr\npushfq\npopq %3"
                   : "+a"(Low), "+d"(High), "+c"(Index), "=r"(Flags)
                   :
                   : "cc", "memory");
#else
  __asm__ volatile("stc\nrdmsr\npushfq\npopq %3"
                   : "+a"(Low), "+d"(High), "+c"(Index), "=r"(Flags)
                   :
                   : "cc", "memory");
#endif
  if (Low > 0xffffffffULL || High != 0xfffff800ULL ||
      Index != 0x12345678c0000082ULL || !(Flags & 1))
    return (int)0xc000000d;
  const volatile unsigned char *Entry =
      (const volatile unsigned char *)((High << 32) | Low);
  return Entry[0] == 0x0f && Entry[1] == 0x01 && Entry[2] == 0xf8
             ? 0
             : (int)0xc000000d;
}
