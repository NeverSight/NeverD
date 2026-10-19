// A boot-fixed clock fact survives IRQL changes and virtual-time advancement.
typedef unsigned int U32;
__declspec(dllimport) U32 KeQueryTimeIncrement(void);
__declspec(dllimport) void KeStallExecutionProcessor(U32);
__declspec(dllimport) unsigned char KfRaiseIrql(unsigned char);
__declspec(dllimport) void KeLowerIrql(unsigned char);
__declspec(dllimport) int KeDelayExecutionThread(U32, unsigned char,
                                                 const long long *);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 Before = KeQueryTimeIncrement();
  unsigned char Previous = KfRaiseIrql(2);
  KeStallExecutionProcessor(25);
  U32 Raised = KeQueryTimeIncrement();
  KeLowerIrql(Previous);
#ifdef CHECK_SHARED_TICKS
  typedef unsigned long long U64;
  U64 BeforeTicks = *(volatile U64 *)0xfffff78000000320ULL;
  long long Delay = -2000000;
#else
  long long Delay = -100000;
#endif
  if (KeDelayExecutionThread(0, 0, &Delay))
    return (int)0xc000000d;
  U32 After = KeQueryTimeIncrement();
  if (Before != 156250 || Raised != Before || After != Before)
    return (int)0xc000000d;
#ifdef CHECK_SHARED_TICKS
  U64 AfterTicks = *(volatile U64 *)0xfffff78000000320ULL;
  if (AfterTicks - BeforeTicks != 12)
    return (int)0xc000000d;
#endif
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
