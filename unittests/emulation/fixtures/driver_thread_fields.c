// Native-layout client for driver_kernel_debugger_reference.c. Its private
// offsets deliberately differ from the host kernel and require the companion.
typedef unsigned long long U64;
typedef unsigned int U32;
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void *PsGetCurrentThreadId(void);
__declspec(dllimport) void *PsGetThreadProcessId(void *);
__declspec(dllimport) void *PsGetThreadProcess(void *);
__declspec(dllimport) void *IoThreadToProcess(void *);
__declspec(dllimport) void *IoGetCurrentProcess(void);
__declspec(dllimport) int ZwQueryInformationThread(void *, U32, void *, U32,
                                                   U32 *);
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  volatile unsigned char *Thread = PsGetCurrentThread();
  void *Start = (void *)0;
  U32 Length = 0;
  if (ZwQueryInformationThread((void *)~1ULL, 9, &Start, sizeof(Start),
                               &Length))
    return (int)0xc000000d;
  if (*(volatile U64 *)(Thread + 0x190) !=
          (U64)PsGetThreadProcessId((void *)Thread) ||
      *(volatile U64 *)(Thread + 0x198) != (U64)PsGetCurrentThreadId() ||
      *(volatile U64 *)(Thread + 0x1c0) !=
          (U64)PsGetThreadProcess((void *)Thread) ||
      *(volatile U64 *)(Thread + 0x1c0) !=
          (U64)IoThreadToProcess((void *)Thread) ||
      *(volatile U64 *)(Thread + 0x1c0) != (U64)IoGetCurrentProcess() ||
      *(volatile U64 *)(Thread + 0x1e0) != (U64)Start ||
      Start != (void *)&DriverEntry || Length != 8)
    return (int)0xc000000d;
  return 0;
}
