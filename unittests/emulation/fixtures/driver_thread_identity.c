// Original client: public CIDs, GS current identity and handle references
// agree.
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  void *Process, *Thread;
} CLIENT_ID;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
__declspec(dllimport) int PsCreateSystemThread(void **, U32, void *, void *,
                                               void *, void (*)(void *),
                                               void *);
__declspec(dllimport) int ObReferenceObjectByHandle(void *, U32, void *, U32,
                                                    void **, void *);
__declspec(dllimport) int ObOpenObjectByPointer(void *, U32, void *, U32,
                                                void *, U32, void **);
__declspec(dllimport) int ZwQueryInformationThread(void *, U32, void *, U32,
                                                   U32 *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) int ZwClose(void *);
__declspec(dllimport) int KeWaitForSingleObject(void *, U32, U32, U32, void *);
__declspec(dllimport) void PsTerminateSystemThread(U32);
__declspec(dllimport) void *PsThreadType;
__declspec(dllimport) void *IoFileObjectType;
__declspec(dllimport) void *PsGetThreadProcessId(void *);
__declspec(dllimport) void *PsGetThreadProcess(void *);
__declspec(dllimport) void *IoThreadToProcess(void *);
__declspec(dllimport) void *IoGetCurrentProcess(void);
static void *(*Current)(void);
static void *(*CurrentID)(void);
static void *(*GetID)(void *);
static int (*Lookup)(void *, void **);
static void *(*CurrentWin32)(void);
static void *(*GetWin32)(void *);
static void (*SetWin32)(void *, void *, void *);
static void *RootID;
static volatile U32 ChildChecked;
static CLIENT_ID Created;
static void *Resolve(const U16 *Text) {
  STRING Name;
  RtlInitUnicodeString(&Name, Text);
  return MmGetSystemRoutineAddress(&Name);
}
static void *GSCurrent(void) {
  void *Object;
  __asm__("movq %%gs:0x188, %0" : "=r"(Object));
  return Object;
}
static void Child(void *Context) {
  void *Object = Current(), *Found = (void *)0;
  void *Start = (void *)0;
  U32 Length = 0;
  if (Context && !CurrentWin32() && !GetWin32(Object) &&
      Object == GSCurrent() && CurrentID() != RootID &&
      CurrentID() == Created.Thread && Created.Process == (void *)4 &&
      GetID(Object) == CurrentID() && !Lookup(CurrentID(), &Found) &&
      Found == Object && PsGetThreadProcessId(Object) == (void *)4 &&
      PsGetThreadProcess(Object) == IoGetCurrentProcess() &&
      IoThreadToProcess(Object) == IoGetCurrentProcess() &&
      !ZwQueryInformationThread((void *)~1ULL, 9, &Start, sizeof(Start),
                                &Length) &&
      Start == (void *)&Child && Length == 8) {
    ObfDereferenceObject(Found);
    ChildChecked = 1;
  }
  PsTerminateSystemThread(0);
}
int DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  Current = Resolve((const U16 *)L"PsGetCurrentThread");
  CurrentID = Resolve((const U16 *)L"PsGetCurrentThreadId");
  GetID = Resolve((const U16 *)L"PsGetThreadId");
  Lookup = Resolve((const U16 *)L"PsLookupThreadByThreadId");
  CurrentWin32 = Resolve((const U16 *)L"PsGetCurrentThreadWin32Thread");
  GetWin32 = Resolve((const U16 *)L"PsGetThreadWin32Thread");
  SetWin32 = Resolve((const U16 *)L"PsSetThreadWin32Thread");
  if (!Current || !CurrentID || !GetID || !Lookup || !CurrentWin32 ||
      !GetWin32 || !SetWin32)
    return (int)0xc000000d;
  RootID = CurrentID();
  void *Object = Current(), *Found = (void *)0;
  if (!RootID || Object != GSCurrent() || GetID(Object) != RootID ||
      PsGetThreadProcessId(Object) != (void *)4 ||
      PsGetThreadProcess(Object) != IoGetCurrentProcess() ||
      IoThreadToProcess(Object) != IoGetCurrentProcess() ||
      Lookup(RootID, &Found) || Found != Object)
    return (int)0xc000000d;
  ObfDereferenceObject(Found);
  if (CurrentWin32() || GetWin32(Object))
    return (int)0xc000000d;
  SetWin32(Object, Driver, (void *)0);
  SetWin32(Object, (void *)0, Object);
  if (CurrentWin32() != Driver || GetWin32(Object) != Driver)
    return (int)0xc000000d;
  Found = (void *)0x1234;
  if ((U32)Lookup((void *)((U64)RootID + 0x100000000ULL), &Found) !=
          0xc000000b ||
      Found != (void *)0x1234)
    return (int)0xc000000d;
  void *RootHandle = (void *)0, *RootPeer = (void *)0;
  if (ObOpenObjectByPointer(Object, 0x200, (void *)0, 0x40, PsThreadType, 0,
                            &RootHandle) ||
      ObOpenObjectByPointer(Object, 0, (void *)0, 0, (void *)0, 0, &RootPeer) ||
      RootHandle == RootPeer || ZwClose(RootHandle) ||
      (U32)ZwClose(RootHandle) != 0xc0000008 ||
      ObReferenceObjectByHandle(RootPeer, 0, PsThreadType, 0, &Found,
                                (void *)0) ||
      Found != Object)
    return (int)0xc000000d;
  ObfDereferenceObject(Found);
  if (ZwClose(RootPeer))
    return (int)0xc000000d;
  void *Start = (void *)0;
  U32 Length = 0;
  if (ZwQueryInformationThread((void *)~1ULL, 9, &Start, sizeof(Start),
                               &Length) ||
      Start != (void *)&DriverEntry || Length != 8)
    return (int)0xc000000d;
  void *Handle = (void *)0, *Thread = (void *)0;
  if (!PsThreadType || PsThreadType == IoFileObjectType ||
      PsCreateSystemThread(&Handle, 0x10000000, (void *)0, (void *)0, &Created,
                           Child, Driver) ||
      ObReferenceObjectByHandle(Handle, 0, PsThreadType, 0, &Thread, (void *)0))
    return (int)0xc000000d;
  void *ID = GetID(Thread);
  if (!ID || ID == RootID || ID != Created.Thread ||
      Created.Process != (void *)4 || Lookup(ID, &Found) || Found != Thread)
    return (int)0xc000000d;
  ObfDereferenceObject(Found);
  void *Peer = (void *)0;
  if (ObOpenObjectByPointer(Thread, 0x200, (void *)0, 0, PsThreadType, 0,
                            &Peer))
    return (int)0xc000000d;
  if (ZwClose(Handle) || KeWaitForSingleObject(Thread, 0, 0, 0, (void *)0) ||
      !ChildChecked || GetID(Thread) != ID ||
      PsGetThreadProcessId(Thread) != (void *)4 ||
      IoThreadToProcess(Thread) != IoGetCurrentProcess() ||
      (U32)Lookup(ID, &Found) != 0xc000000b ||
      ZwQueryInformationThread(Peer, 9, &Start, sizeof(Start), &Length) ||
      Start != (void *)&Child || Length != 8 || ZwClose(Peer))
    return (int)0xc000000d;
  ObfDereferenceObject(Thread);
  if (CurrentID() != RootID || Current() != Object || CurrentWin32() != Driver)
    return (int)0xc000000d;
  SetWin32(Object, (void *)0, Driver);
  return !CurrentWin32() && !GetWin32(Object) ? 0 : (int)0xc000000d;
}
