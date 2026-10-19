typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) int NtQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) int PsLookupProcessByProcessId(void *, void **);
__declspec(dllimport) int PsLookupThreadByThreadId(void *, void **);
__declspec(dllimport) void *PsGetProcessId(void *);
__declspec(dllimport) const unsigned char *PsGetProcessImageFileName(void *);
__declspec(dllimport) void *PsGetThreadId(void *);
__declspec(dllimport) void *PsGetCurrentThreadId(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static U64 Read64(const unsigned char *B, U32 At) {
  return *(const U64 *)(B + At);
}
static U32 Read32(const unsigned char *B, U32 At) {
  return *(const U32 *)(B + At);
}
static void Unload(void *Driver) { (void)Driver; }
__declspec(align(8)) static unsigned char Buffer[4096];
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 Required = 0;
  if ((U32)ZwQuerySystemInformation(5, &Required, 0, &Required) != 0xc0000004 ||
      Required < 0x200 || Required > sizeof(Buffer))
    return (int)0xc000000d;
  U32 Captured = Required;
  if ((U32)ZwQuerySystemInformation(5, Buffer, Required - 1, &Required) !=
          0xc0000004 ||
      Required != Captured)
    return (int)0xc000000d;
  if (NtQuerySystemInformation(5, Buffer, sizeof(Buffer), &Required))
    return (int)0xc000000d;
  U32 Offset = 0, Count = 0, Threads = 0;
  for (;;) {
    const unsigned char *Row = Buffer + Offset;
    U32 Next = Read32(Row, 0), Number = Read32(Row, 4);
    U64 PID = Read64(Row, 0x50);
    U16 NameSize = *(const U16 *)(Row + 0x38);
    const U16 *Name = (const U16 *)Read64(Row, 0x40);
    if (++Count > 2 || (PID != 4 && PID != 100) || !NameSize ||
        (NameSize & 1) ||
        (const unsigned char *)Name < Row + 0x100 + Number * 0x50 ||
        (const unsigned char *)(Name + NameSize / 2 + 1) > Buffer + Required ||
        Name[NameSize / 2] != 0)
      return (int)0xc000000d;
    if ((PID == 4 && (NameSize != 12 || Name[0] != 'S')) ||
        (PID == 100 && (NameSize != 20 || Name[0] != 'C')))
      return (int)0xc000000d;
    void *Process = 0;
    if (PsLookupProcessByProcessId((void *)PID, &Process) ||
        (U64)PsGetProcessId(Process) != PID)
      return (int)0xc000000d;
    const unsigned char *Short = PsGetProcessImageFileName(Process);
    if ((U64)Short != (U64)Process + 0x5a8 ||
        Short[0] != (PID == 4 ? 'S' : 'C') || Short[PID == 4 ? 6 : 10] != 0)
      return (int)0xc000000d;
    ObfDereferenceObject(Process);
    for (U32 T = 0; T < Number; ++T) {
      const unsigned char *Thread = Row + 0x100 + T * 0x50;
      U64 TID = Read64(Thread, 0x30);
      void *Object = 0;
      if (Read64(Thread, 0x28) != PID ||
          PsLookupThreadByThreadId((void *)TID, &Object) ||
          (U64)PsGetThreadId(Object) != TID ||
          (TID == (U64)PsGetCurrentThreadId() &&
           Object != PsGetCurrentThread()))
        return (int)0xc000000d;
      ObfDereferenceObject(Object);
      ++Threads;
    }
    if (!Next)
      break;
    if ((Next & 7) || Next < 0x100 + Number * 0x50 || Next >= Required - Offset)
      return (int)0xc000000d;
    Offset += Next;
  }
  if (Count != 2 || Threads != 1)
    return (int)0xc000000d;
  DbgPrint("process snapshot passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
