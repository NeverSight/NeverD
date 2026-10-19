typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
__declspec(dllimport) void *PsInitialSystemProcess;
__declspec(dllimport) void *IoGetCurrentProcess(void);
__declspec(dllimport) void *PsGetProcessId(void *);
__declspec(dllimport) int PsLookupProcessByProcessId(void *, void **);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const NAME *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  const U16 Text[] = {'P', 's', 'I', 'n', 'i', 't', 'i', 'a',
                      'l', 'S', 'y', 's', 't', 'e', 'm', 'P',
                      'r', 'o', 'c', 'e', 's', 's', 0};
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  void **Slot = (void **)MmGetSystemRoutineAddress(&Name);
  if (!Slot || Slot != &PsInitialSystemProcess)
    return (int)0xc000000d;
#if defined(PROCESS_DATA_WRITE)
  *Slot = 0;
#elif defined(PROCESS_DATA_EXECUTE)
  ((void (*)(void))Slot)();
#elif defined(PROCESS_DATA_CROSS)
  if (*(volatile U64 *)((char *)Slot + 1))
    return 1;
#endif
  void *System = *Slot, *LookedUp = 0;
  if (!System || System != IoGetCurrentProcess() ||
      (U64)PsGetProcessId(System) != 4 ||
      PsLookupProcessByProcessId((void *)4, &LookedUp) || LookedUp != System)
    return (int)0xc000000d;
  ObfDereferenceObject(LookedUp);
  if (PsInitialSystemProcess != System || *Slot != System)
    return (int)0xc000000d;
  DbgPrint("System process data passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
