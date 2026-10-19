typedef unsigned long long U64;
typedef unsigned int U32;
typedef unsigned short U16;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
#define API __declspec(dllimport)
API void *PsGetProcessSectionBaseAddress(void *);
API U32 PsLookupProcessByProcessId(U64, void **);
API void *IoGetCurrentProcess(void);
API void ObfDereferenceObject(void *);
API void *MmGetSystemRoutineAddress(const NAME *);
API void KeStackAttachProcess(void *, void *);
API void KeUnstackDetachProcess(void *);
API U32 PsSetCreateProcessNotifyRoutine(void (*)(U64, U64, unsigned char),
                                        unsigned char);
API U32 DbgPrint(const char *, ...);
API U32 IoCreateDevice(void *, U32, const NAME *, U32, U32, unsigned char,
                       void **);
API void IoDeleteDevice(void *);
API void *IoAllocateWorkItem(void *);
API void IoQueueWorkItem(void *, void (*)(void *, void *), U32, void *);
API void IoFreeWorkItem(void *);
API void IoMarkIrpPending(void *);
API void IofCompleteRequest(void *, unsigned char);
static void *User, *Device, *Work;
static U32 Count, Bad, WorkCompleted;
static void *(*Get)(void *);
static void Worker(void *Object, void *Context) {
  (void)Object;
  (void)Context;
  U64 State[6] = {0};
  KeStackAttachProcess(User, State);
  U64 Base = (U64)Get(IoGetCurrentProcess());
  KeUnstackDetachProcess(State);
  if (Base != 0x140000000)
    Bad = 1;
  IoFreeWorkItem(Work);
  WorkCompleted = 1;
  *(U32 *)((char *)Context + 0x30) = Bad ? 0xc000000d : 0;
  *(U64 *)((char *)Context + 0x38) = 0;
  IofCompleteRequest(Context, 0);
}
static U32 Dispatch(void *Object, void *Request) {
  (void)Object;
  IoMarkIrpPending(Request);
  IoQueueWorkItem(Work, Worker, 1, Request);
  return 0x103;
}
static U32 Close(void *Object, void *Request) {
  (void)Object;
  *(U32 *)((char *)Request + 0x30) = 0;
  *(U64 *)((char *)Request + 0x38) = 0;
  IofCompleteRequest(Request, 0);
  return 0;
}
static void Notify(U64 Parent, U64 PID, unsigned char Create) {
  void *Process = 0;
  if (Parent != 100 || PID != 200 || Create != (Count == 0) ||
      PsLookupProcessByProcessId(PID, &Process)) {
    Bad = 1;
    return;
  }
  if ((U64)PsGetProcessSectionBaseAddress(Process) != 0x400000 ||
      *(volatile U64 *)((char *)Process + 0x520) != 0x400000)
    Bad = 1;
  ObfDereferenceObject(Process);
  ++Count;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Bad || !WorkCompleted || (Count && Count != 2) ||
      PsSetCreateProcessNotifyRoutine(Notify, 1))
    *(volatile U32 *)0 = 0xbad;
  ObfDereferenceObject(User);
  IoDeleteDevice(Device);
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  if (PsGetProcessSectionBaseAddress(IoGetCurrentProcess()) != 0 ||
      PsLookupProcessByProcessId(100, &User) ||
      (U64)PsGetProcessSectionBaseAddress(User) != 0x140000000 ||
      *(volatile U64 *)((char *)User + 0x520) != 0x140000000)
    return (int)0xc000000d;
  const U16 Text[] = L"PsGetProcessSectionBaseAddress";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  Get = (void *(*)(void *))MmGetSystemRoutineAddress(&Name);
  if (!Get || (U64)Get(User) != 0x140000000)
    return (int)0xc000000d;
  if (IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device) ||
      !(Work = IoAllocateWorkItem(Device)) ||
      PsSetCreateProcessNotifyRoutine(Notify, 0))
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x70) = Dispatch;
  *(void **)((char *)Driver + 0x70 + 2 * 8) = Close;
  *(void **)((char *)Driver + 0x70 + 18 * 8) = Close;
  DbgPrint("process section bases passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
