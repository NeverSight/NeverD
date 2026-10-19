// Original client: compare reference VA assignments with real pool and MDL VAs.
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void *IoAllocateMdl(void *, U32, unsigned char,
                                          unsigned char, void *);
__declspec(dllimport) void MmBuildMdlForNonPagedPool(void *);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(void *, U32, U32,
                                                         void *, U32, U32);
__declspec(dllimport) void MmUnmapLockedPages(void *, void *);
__declspec(dllimport) void IoFreeMdl(void *);
__declspec(dllimport) int PsLookupProcessByProcessId(void *, void **);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void *PsGetProcessId(void *);
static const U16 AccessorName[] = {'M', 'i', 'Q', 'u', 'e', 'r', 'y', 'S', 'y',
                                   's', 't', 'e', 'm', 'B', 'a', 's', 'e', 0};
static const U16 UnsecureName[] = {'M', 'm', 'U', 'n', 's', 'e', 'c', 'u',
                                   'r', 'e', 'V', 'i', 'r', 't', 'u', 'a',
                                   'l', 'M', 'e', 'm', 'o', 'r', 'y', 0};
static volatile U64 *Assignments;
static const U16 NextProcessName[] = {'P', 's', 'G', 'e', 't', 'N',
                                      'e', 'x', 't', 'P', 'r', 'o',
                                      'c', 'e', 's', 's', 0};
static U32 Classify(const void *Pointer) {
  for (U32 I = 0; I != 13; ++I) {
    U64 Base = Assignments[I * 2], Size = Assignments[I * 2 + 1];
    if ((U64)Pointer >= Base && (U64)Pointer - Base < Size)
      return I;
  }
  return 13;
}
int DriverEntry(void *Driver, void *Registry) {
  (void)Driver;
  (void)Registry;
  STRING Name;
  RtlInitUnicodeString(&Name, AccessorName);
  unsigned char *Code = MmGetSystemRoutineAddress(&Name);
  if (!Code)
    return (int)0xc000000d;
  Assignments = (void *)(Code + 10 + *(int *)(Code + 6));
  if (Classify(Code) != 3 || Classify((void *)&DriverEntry) != 12)
    return (int)0xc000000d;
#ifdef WRITE_SYSTEMVA
  Assignments[18] = 0;
#endif
  void *Nonpaged = ExAllocatePoolWithTag(0, 64, 0x53564131);
  void *Paged = ExAllocatePoolWithTag(1, 64, 0x53564132);
  if (!Nonpaged || !Paged || Classify(Nonpaged) != 5 || Classify(Paged) != 6)
    return (int)0xc000000d;
  void *MDL = IoAllocateMdl(Nonpaged, 64, 0, 0, (void *)0);
  MmBuildMdlForNonPagedPool(MDL);
  void *Alias = MmMapLockedPagesSpecifyCache(MDL, 0, 1, (void *)0, 0, 16);
  if (!Alias || Classify(Alias) != 9)
    return (int)0xc000000d;
  MmUnmapLockedPages(Alias, MDL);
  IoFreeMdl(MDL);
  ExFreePoolWithTag(Nonpaged, 0x53564131);
  ExFreePoolWithTag(Paged, 0x53564132);
  RtlInitUnicodeString(&Name, UnsecureName);
  Code = MmGetSystemRoutineAddress(&Name);
  if (!Code)
    return (int)0xc000000d;
  volatile U64 *Cookie = (void *)(Code + 38 + *(int *)(Code + 34));
  U64 First = *Cookie;
  if (!First || *Cookie != First)
    return (int)0xc000000d;
  RtlInitUnicodeString(&Name, NextProcessName);
  Code = MmGetSystemRoutineAddress(&Name);
  if (!Code)
    return (int)0xc000000d;
  volatile U64 *Head = (void *)(Code + 69 + *(int *)(Code + 65));
  void *System = (void *)0;
  if (PsLookupProcessByProcessId((void *)4, &System))
    return (int)0xc000000d;
  ObfDereferenceObject(System);
  U64 Offset = Head[0] - (U64)System;
  if (!Offset || Offset > 0x800 || PsGetProcessId(System) != (void *)4)
    return (int)0xc000000d;
  volatile U64 *Link = (void *)((U64)System + Offset);
  if (Head[1] != (U64)Link || Link[0] != (U64)Head || Link[1] != (U64)Head)
    return (int)0xc000000d;
  return 0;
}
