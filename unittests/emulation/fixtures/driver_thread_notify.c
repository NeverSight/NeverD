typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U32 PsSetCreateThreadNotifyRoutine(void (*)(U64, U64, U8));
API U32 PsRemoveCreateThreadNotifyRoutine(void (*)(U64, U64, U8));
API U32 PsCreateSystemThread(void **, U32, void *, void *, void *,
                             void (*)(void *), void *);
API void PsTerminateSystemThread(U32);
API U64 PsGetCurrentProcessId(void);
API U64 PsGetCurrentThreadId(void);
API void *PsGetCurrentThread(void);
API U64 PsGetThreadId(void *);
API U64 PsGetThreadProcessId(void *);
API U8 PsIsThreadTerminating(void *);
API U8 KeAreApcsDisabled(void);
API U8 KeAreAllApcsDisabled(void);
API U8 KeGetCurrentIrql(void);
API U32 PsLookupThreadByThreadId(U64, void **);
API void ObfDereferenceObject(void *);
API U32 ObReferenceObjectByHandle(void *, U32, void *, U32, void **, void *);
API U32 KeWaitForSingleObject(void *, U32, U32, U32, void *);
API U32 ZwClose(void *);
API void *MmGetSystemRoutineAddress(const void *);
API U32 DbgPrint(const char *, ...);
static U64 Creator, Target;
static volatile U32 Created, Deleted, Ran, Error, Returned;
static void *Handle;
static U64 Client[2];
static void Notify(U64 PID, U64 TID, U8 Create) {
  if (PID != 4 || !KeAreApcsDisabled() || KeAreAllApcsDisabled() ||
      KeGetCurrentIrql())
    Error = 1;
  if (Create) {
    if (PsGetCurrentThreadId() != Creator || TID == Creator || Ran ||
        Handle != (void *)0x1122334455667788ULL)
      Error = 2;
    if (!Target)
      Target = TID;
    if (Target != TID)
      Error = 3;
    ++Created;
  } else {
    if (PsGetCurrentThreadId() != TID || TID != Target || !Ran ||
        Created != 2 || !PsIsThreadTerminating(PsGetCurrentThread()))
      Error = 4;
    ++Deleted;
  }
  void *Thread = 0;
  if (PsLookupThreadByThreadId(TID, &Thread) || !Thread)
    Error = 5;
  else {
    if (PsGetThreadId(Thread) != TID || PsGetThreadProcessId(Thread) != 4)
      Error = 6;
    ObfDereferenceObject(Thread);
  }
}
static void Child(void *Context) {
  (void)Context;
  if (Created != 2 || Error || PsGetCurrentThreadId() != Target ||
      PsGetCurrentProcessId() != 4)
    Error = 7;
  Ran = 1;
  PsTerminateSystemThread(Error ? 0xc000000d : 0);
  Returned = 1;
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  Creator = PsGetCurrentThreadId();
  const U16 Text[] = {'P', 's', 'S', 'e', 't', 'C', 'r', 'e', 'a', 't', 'e',
                      'T', 'h', 'r', 'e', 'a', 'd', 'N', 'o', 't', 'i', 'f',
                      'y', 'R', 'o', 'u', 't', 'i', 'n', 'e', 0};
  struct {
    U16 Length, Maximum;
    const U16 *Buffer;
  } Name = {60, 62, Text};
  U32 (*Set)(void (*)(U64, U64, U8)) = MmGetSystemRoutineAddress(&Name);
  if (!Set || PsSetCreateThreadNotifyRoutine(Notify) || Set(Notify))
    return (int)0xc000000d;
  Handle = (void *)0x1122334455667788ULL;
  if (PsCreateSystemThread(&Handle, 0x10000000, 0, 0, Client, Child, 0) ||
      Created != 2 || Client[0] != 4 || Client[1] != Target)
    return (int)0xc000000d;
  void *Thread = 0;
  if (ObReferenceObjectByHandle(Handle, 0, 0, 0, &Thread, 0) ||
      KeWaitForSingleObject(Thread, 0, 0, 0, 0) || Deleted != 2 || !Ran ||
      Error || Returned)
    return (int)0xc000000d;
  ObfDereferenceObject(Thread);
  if (ZwClose(Handle) || PsRemoveCreateThreadNotifyRoutine(Notify) ||
      PsRemoveCreateThreadNotifyRoutine(Notify) ||
      PsRemoveCreateThreadNotifyRoutine(Notify) != 0xc000007a)
    return (int)0xc000000d;
  DbgPrint("thread creation, exit, CID and callback ordering passed");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
