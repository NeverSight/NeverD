typedef unsigned long long U64;
typedef unsigned int U32;
typedef unsigned short U16;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
#define API __declspec(dllimport)
API void *PsGetProcessPeb(void *);
API U32 PsLookupProcessByProcessId(U64, void **);
API void *IoGetCurrentProcess(void);
API void ObfDereferenceObject(void *);
API void *MmGetSystemRoutineAddress(const NAME *);
API void KeStackAttachProcess(void *, void *);
API void KeUnstackDetachProcess(void *);
API U32 IoCreateDevice(void *, U32, const NAME *, U32, U32, unsigned char,
                       void **);
API void IoDeleteDevice(void *);
API U32 DbgPrint(const char *, ...);
API void IofCompleteRequest(void *, unsigned char);
static void *A, *B, *Device;
static void *(*Get)(void *);
static U32 Check(void *Process, U64 Base, unsigned char Marker, int Change) {
  U64 State[6] = {0};
  KeStackAttachProcess(Process, State);
  volatile unsigned char *Peb = Get(IoGetCurrentProcess());
  U32 Bad = (U64)Peb != 0x400000 || Peb[2] != Marker;
  if (!Bad) {
    U64 Ldr = *(volatile U64 *)(Peb + 0x18);
    U64 Entry = *(volatile U64 *)(Ldr + 0x10);
    Bad = *(volatile U64 *)(Peb + 0x10) != Base ||
          *(volatile U64 *)(Entry + 0x30) != Base ||
          *(volatile U64 *)Entry != Ldr + 0x10 ||
          *(volatile U64 *)(Entry + 8) != Ldr + 0x10;
    if (Change)
      Peb[2] = 0xa7;
  }
  KeUnstackDetachProcess(State);
  return Bad;
}
static U32 Dispatch(void *Object, void *Request) {
  (void)Object;
  U32 Bad = Get(IoGetCurrentProcess()) != (void *)0x400000 ||
            *(volatile unsigned char *)0x400002 != 0xa7;
  *(U32 *)((char *)Request + 0x30) = Bad ? 0xc000000d : 0;
  *(U64 *)((char *)Request + 0x38) = 0;
  IofCompleteRequest(Request, 0);
  return Bad ? 0xc000000d : 0;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Check(A, 0x140000000, 0xa7, 0) || Check(B, 0x180000000, 0xb1, 0))
    *(volatile U32 *)0 = 0xbad;
  ObfDereferenceObject(A);
  ObfDereferenceObject(B);
  IoDeleteDevice(Device);
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  if (PsGetProcessPeb(IoGetCurrentProcess()) ||
      *(volatile U64 *)((char *)IoGetCurrentProcess() + 0x550) ||
      PsLookupProcessByProcessId(100, &A) ||
      PsLookupProcessByProcessId(200, &B) ||
      PsGetProcessPeb(A) != (void *)0x400000 ||
      *(volatile U64 *)((char *)A + 0x550) != 0x400000)
    return (int)0xc000000d;
  const U16 Text[] = L"PsGetProcessPeb";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  Get = (void *(*)(void *))MmGetSystemRoutineAddress(&Name);
  if (!Get || Check(A, 0x140000000, 0xa1, 1) ||
      Check(B, 0x180000000, 0xb1, 0) ||
      IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device))
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x70) = Dispatch;
  *(void **)((char *)Driver + 0x70 + 2 * 8) = Dispatch;
  *(void **)((char *)Driver + 0x70 + 18 * 8) = Dispatch;
  *(void **)((char *)Driver + 0x68) = Unload;
  DbgPrint("process PEB observations passed");
  return 0;
}
