typedef unsigned long long U64;
typedef unsigned int U32;
typedef unsigned short U16;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
#define API __declspec(dllimport)
API U32 PsGetProcessExitStatus(void *);
API U32 PsLookupProcessByProcessId(U64, void **);
API void *IoGetCurrentProcess(void);
API void ObfDereferenceObject(void *);
API void *MmGetSystemRoutineAddress(const NAME *);
API U32 PsSetCreateProcessNotifyRoutine(void (*)(U64, U64, unsigned char),
                                        unsigned char);
API U32 IoCreateDevice(void *, U32, const NAME *, U32, U32, unsigned char,
                       void **);
API void IoDeleteDevice(void *);
API void *IoAllocateWorkItem(void *);
API void IoQueueWorkItem(void *, void (*)(void *, void *), U32, void *);
API void IoFreeWorkItem(void *);
API void IoMarkIrpPending(void *);
API void IofCompleteRequest(void *, unsigned char);
API void KeStackAttachProcess(void *, void *);
API void KeUnstackDetachProcess(void *);
API U32 DbgPrint(const char *, ...);
static void *Held, *Device, *Work;
static U32 Bad, Count, WorkCompleted;
static U32 (*Get)(void *);
static void Notify(U64 Parent, U64 PID, unsigned char Create) {
  if (Parent != 4 || PID != 100 || Create != (Count == 0))
    Bad = 1;
  if (Create && PsLookupProcessByProcessId(PID, &Held)) {
    Bad = 1;
    return;
  }
  U32 Expected = Create ? 0x103 : 0xc0000005;
  if (!Held || Get(Held) != Expected ||
      *(volatile U32 *)((char *)Held + 0x7d4) != Expected)
    Bad = 1;
  ++Count;
}
static void Worker(void *Object, void *Request) {
  (void)Object;
  U64 State[6] = {0};
  KeStackAttachProcess(Held, State);
  U32 Status = Get(IoGetCurrentProcess());
  KeUnstackDetachProcess(State);
  if (Status != 0x103)
    Bad = 1;
  IoFreeWorkItem(Work);
  WorkCompleted = 1;
  *(U32 *)((char *)Request + 0x30) = Bad ? 0xc000000d : 0;
  *(U64 *)((char *)Request + 0x38) = 0;
  IofCompleteRequest(Request, 0);
}
static U32 Create(void *Object, void *Request) {
  (void)Object;
  IoMarkIrpPending(Request);
  IoQueueWorkItem(Work, Worker, 1, Request);
  return 0x103;
}
static U32 Complete(void *Object, void *Request) {
  (void)Object;
  *(U32 *)((char *)Request + 0x30) = 0;
  *(U64 *)((char *)Request + 0x38) = 0;
  IofCompleteRequest(Request, 0);
  return 0;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Bad || Count != 2 || !WorkCompleted || Get(Held) != 0xc0000005 ||
      *(volatile U32 *)((char *)Held + 0x7d4) != 0xc0000005 ||
      PsSetCreateProcessNotifyRoutine(Notify, 1))
    *(volatile U32 *)0 = 0xbad;
  ObfDereferenceObject(Held);
  IoDeleteDevice(Device);
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  if (PsGetProcessExitStatus(IoGetCurrentProcess()) != 0x103)
    return (int)0xc000000d;
  const U16 Text[] = L"PsGetProcessExitStatus";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  Get = (U32 (*)(void *))MmGetSystemRoutineAddress(&Name);
  if (!Get || PsSetCreateProcessNotifyRoutine(Notify, 0) ||
      IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device) ||
      !(Work = IoAllocateWorkItem(Device)))
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x70) = Create;
  *(void **)((char *)Driver + 0x70 + 2 * 8) = Complete;
  *(void **)((char *)Driver + 0x70 + 18 * 8) = Complete;
  *(void **)((char *)Driver + 0x68) = Unload;
  DbgPrint("process exit status lifecycle passed");
  return 0;
}
