// Original x64 client scans its own initialized System-thread body.
typedef unsigned long long U64;
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void *IoGetCurrentProcess(void);
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  volatile unsigned char *Thread = PsGetCurrentThread();
#ifdef WRITE_THREAD
  Thread[0] = 0;
#endif
  if (Thread[0] != 6 || Thread[0x184] != 2)
    return (int)0xc000000d;
  const volatile unsigned char *Process = IoGetCurrentProcess();
  const volatile U64 *Head = (const volatile U64 *)(Process + 0x30);
  if (Head[0] != (U64)Thread + 0x2f8 || Head[1] != Head[0] ||
      *(volatile U64 *)(Thread + 0x2f8) != (U64)Head)
    return (int)0xc000000d;
  for (unsigned I = 0; I < 0x800; ++I)
    if (*(volatile U64 *)(Thread + I) == (U64)&DriverEntry)
      return I == 0x450 ? 0 : (int)0xc000000d;
  return (int)0xc0000001;
}
