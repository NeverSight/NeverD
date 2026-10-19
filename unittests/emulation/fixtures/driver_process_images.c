typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U32 PsLookupProcessByProcessId(U64, void **);
API void KeStackAttachProcess(void *, void *);
API void KeUnstackDetachProcess(void *);
API void ObfDereferenceObject(void *);
API void *IoAllocateMdl(void *, U32, U8, U8, void *);
API void MmProbeAndLockPages(void *, U8, U32);
API void *MmMapLockedPagesSpecifyCache(void *, U8, U32, void *, U32, U32);
API void MmUnlockPages(void *);
API void IoFreeMdl(void *);
API U32 ZwQueryVirtualMemory(U64, void *, U32, void *, U64, U64 *);
typedef struct {
  U64 Base, AllocationBase;
  U32 AllocationProtect;
  U16 Partition;
  U64 Size;
  U32 State, Protect, Type;
} BASIC;
typedef struct {
  U16 Length, Maximum;
  U32 Padding;
  U16 *Buffer;
} NAME;
static void Unload(void *Driver) { (void)Driver; }
static U32 query(U64 Address, U32 Protect) {
  BASIC Info = {0};
  if (ZwQueryVirtualMemory(~(U64)0, (void *)Address, 0, &Info, sizeof(Info),
                           0) ||
      Info.AllocationBase != 0x400000 || Info.AllocationProtect != 0x80 ||
      Info.Protect != Protect || Info.Type != 0x1000000 || Info.State != 0x1000)
    return 1;
  return 0;
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  void *A = 0, *B = 0;
  if (PsLookupProcessByProcessId(100, &A) ||
      PsLookupProcessByProcessId(200, &B))
    return (int)0xc000000d;
  U64 StateA[6] = {0}, StateB[6] = {0};
  KeStackAttachProcess(A, StateA);
  if (query(0x400000, 2) || query(0x401000, 0x20) || query(0x402000, 8))
    return (int)0xc000000d;
  U64 Length = 0;
  if (ZwQueryVirtualMemory(~(U64)0, (void *)0x401000, 2, 0, 0, &Length) !=
          0xc0000004 ||
      Length > 512 || Length < 18)
    return (int)0xc000000d;
  U64 NameBuffer[64] = {0};
  if (ZwQueryVirtualMemory(~(U64)0, (void *)0x402000, 2, NameBuffer,
                           sizeof(NameBuffer), &Length))
    return (int)0xc000000d;
  NAME *Name = (NAME *)NameBuffer;
  const U16 Expected[] = L"\\Device\\HarddiskVolume1\\app\\image.exe";
  if (Name->Length != sizeof(Expected) - 2 ||
      Name->Maximum != sizeof(Expected) ||
      Name->Buffer != (U16 *)((U8 *)NameBuffer + 16) ||
      Length != 16 + sizeof(Expected))
    return (int)0xc000000d;
  for (U32 I = 0; I < sizeof(Expected) / 2; ++I)
    if (Name->Buffer[I] != Expected[I])
      return (int)0xc000000d;
  volatile U32 *Data = (volatile U32 *)0x402000;
  void *MDL = IoAllocateMdl((void *)Data, 4096, 0, 0, 0);
  if (!MDL || *Data != 0x31415926)
    return (int)0xc000000d;
  MmProbeAndLockPages(MDL, 1, 0);
  volatile U32 *Alias = MmMapLockedPagesSpecifyCache(MDL, 0, 1, 0, 0, 16);
  if (!Alias || *Alias != 0x31415926)
    return (int)0xc000000d;
  *Data = 0x55667788;
  if (*Data != 0x55667788 || *Alias != 0x31415926 || query(0x402000, 4))
    return (int)0xc000000d;
  KeStackAttachProcess(B, StateB);
  if (*Data != 0x31415926 || query(0x402000, 8))
    return (int)0xc000000d;
  if (__sync_fetch_and_add(Data, 1) != 0x31415926 || *Data != 0x31415927 ||
      query(0x402000, 4))
    return (int)0xc000000d;
  KeUnstackDetachProcess(StateB);
  if (*Data != 0x55667788)
    return (int)0xc000000d;
  KeUnstackDetachProcess(StateA);
  if (*Alias != 0x31415926)
    return (int)0xc000000d;
  MmUnlockPages(MDL);
  IoFreeMdl(MDL);
  ObfDereferenceObject(A);
  ObfDereferenceObject(B);
  *(void **)((U8 *)Driver + 0x68) = Unload;
  return 0;
}
