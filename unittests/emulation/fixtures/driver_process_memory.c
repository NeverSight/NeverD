typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U32 PsLookupProcessByProcessId(U64, void **);
API void *IoGetCurrentProcess(void);
API U64 PsGetCurrentProcessId(void);
API void *PsGetCurrentThread(void);
API U64 PsGetCurrentThreadId(void);
API U8 ExGetPreviousMode(void);
API void KeStackAttachProcess(void *, void *);
API void KeUnstackDetachProcess(void *);
API void ObfDereferenceObject(void *);
API U8 MmIsAddressValid(void *);
API void *IoAllocateMdl(void *, U32, U8, U8, void *);
API void MmProbeAndLockPages(void *, U8, U32);
API void *MmMapLockedPagesSpecifyCache(void *, U8, U32, void *, U32, U32);
API void MmUnlockPages(void *);
API void IoFreeMdl(void *);
API U32 DbgPrint(const char *, ...);
API U32 ZwQueryVirtualMemory(U64, void *, U32, void *, U64, U64 *);
API void *MmGetSystemRoutineAddress(const void *);
typedef struct {
  U64 Base, AllocationBase;
  U32 AllocationProtect;
  U16 Partition;
  U64 Size;
  U32 State, Protect, Type;
} BASIC_MEMORY;
_Static_assert(sizeof(BASIC_MEMORY) == 48, "native AMD64 memory query layout");
typedef U32 (*QUERY_MEMORY)(U64, void *, U32, void *, U64, U64 *);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  void *A = 0, *B = 0;
  void *System = IoGetCurrentProcess(), *Thread = PsGetCurrentThread();
  U64 TID = PsGetCurrentThreadId();
  U8 Mode = ExGetPreviousMode();
  const U16 QueryName[] = {'Z', 'w', 'Q', 'u', 'e', 'r', 'y',
                           'V', 'i', 'r', 't', 'u', 'a', 'l',
                           'M', 'e', 'm', 'o', 'r', 'y', 0};
  struct {
    U16 Length, Maximum;
    const U16 *Buffer;
  } Name = {40, 42, QueryName};
  QUERY_MEMORY NtQuery = (QUERY_MEMORY)MmGetSystemRoutineAddress(&Name);
  if (!NtQuery)
    return (int)0xc0000002;
  if (PsLookupProcessByProcessId(100, &A) ||
      PsLookupProcessByProcessId(200, &B))
    return (int)0xc000000d;
  U64 StateA[6] = {0}, StateB[6] = {0}, StateS[6] = {0};
  volatile U32 *Value = (volatile U32 *)0x400000;
  void *MDL = IoAllocateMdl((void *)Value, 4096, 0, 0, 0);
  if (!MDL)
    return (int)0xc0000017;
  KeStackAttachProcess(A, StateA);
  BASIC_MEMORY Info = {0};
  U64 Length = 0;
  if (ZwQueryVirtualMemory(~(U64)0, (void *)0x400137, 0, &Info, sizeof(Info),
                           &Length) ||
      Info.Base != 0x400000 || Info.AllocationBase != 0x400000 ||
      Info.Size != 4096 || Info.AllocationProtect != 4 || Info.Protect != 4 ||
      Info.State != 0x1000 || Info.Type != 0x20000 || Length != 48)
    return (int)0xc000000d;
  if (NtQuery(~(U64)0, (void *)0x500000, 0, &Info, sizeof(Info), 0) ||
      Info.Type != 0x20000 || Info.Protect != 2)
    return (int)0xc000000d;
  Length = 0x1122334455667788;
  if (ZwQueryVirtualMemory(~(U64)0, (void *)0x400000, 0, &Info, 47, &Length) !=
          0xc0000004 ||
      Length != 0x1122334455667788)
    return (int)0xc000000d;
  if (ZwQueryVirtualMemory(~(U64)0, (void *)0x400000, 0, &Info, sizeof(Info),
                           &Info.Base) ||
      Info.Base != 48)
    return (int)0xc000000d;
  if (IoGetCurrentProcess() != A || PsGetCurrentProcessId() != 4 ||
      PsGetCurrentThread() != Thread || PsGetCurrentThreadId() != TID ||
      ExGetPreviousMode() != Mode || *Value != 0x11111111 ||
      *(volatile U16 *)0x500000 != 0x5a4d || !MmIsAddressValid((void *)Value))
    return (int)0xc000000d;
  *Value = 0x44444444;
  MmProbeAndLockPages(MDL, 1, 0);
  volatile U32 *Alias = MmMapLockedPagesSpecifyCache(MDL, 0, 1, 0, 0, 16);
  if (!Alias || *Alias != 0x44444444)
    return (int)0xc000000d;
  KeStackAttachProcess(B, StateB);
  if (*Value != 0x22222222 || *Alias != 0x44444444 ||
      PsGetCurrentProcessId() != 4)
    return (int)0xc000000d;
  KeUnstackDetachProcess(StateB);
  if (*Value != 0x44444444)
    return (int)0xc000000d;
  KeStackAttachProcess(System, StateS);
  if (IoGetCurrentProcess() != System || MmIsAddressValid((void *)Value))
    return (int)0xc000000d;
  KeUnstackDetachProcess(StateS);
  if (*Value != 0x44444444)
    return (int)0xc000000d;
  KeUnstackDetachProcess(StateA);
  if (IoGetCurrentProcess() != System || MmIsAddressValid((void *)Value) ||
      *Alias != 0x44444444 || PsGetCurrentThread() != Thread)
    return (int)0xc000000d;
  MmUnlockPages(MDL);
  IoFreeMdl(MDL);
  ObfDereferenceObject(A);
  ObfDereferenceObject(B);
  DbgPrint("foreground process memory and nested attachment passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
