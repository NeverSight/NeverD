//===- driver_callbacks.c - Executive callback lifecycle ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U64 Buffer;
} US;
typedef struct {
  U32 Length;
  U64 Root, Name;
  U32 Flags;
  U64 Security, QoS;
} OA;
#define API __declspec(dllimport)
API U32 ExCreateCallback(U64 *, OA *, U8, U8);
API U64 ExRegisterCallback(U64, void (*)(U64, U64, U64), U64);
API void ExUnregisterCallback(U64);
API void ExNotifyCallback(U64, U64, U64);
API void ObfDereferenceObject(U64);
API U8 KfRaiseIrql(U8);
API void KeLowerIrql(U8);
API U8 KeGetCurrentIrql(void);
static U64 Object, PowerObject, PowerRegistration;

typedef struct {
  U64 Data[3];
} EVENT;
API void KeInitializeEvent(EVENT *, U32, U8);
API long KeSetEvent(EVENT *, long, U8);
API U32 KeWaitForSingleObject(void *, U32, U32, U8, long long *);
API U32 PsCreateSystemThread(U64 *, U32, OA *, void *, void *, void (*)(void *),
                             void *);
API void PsTerminateSystemThread(U32);
API U32 ObReferenceObjectByHandle(U64, U32, U64, U8, U64 *, void *);
API U32 ZwClose(U64);
static EVENT Started, WaiterStarted, Continue;
static U64 ConcurrentRegistration;
static volatile U32 UnregisterReturned;

static U32 Log[16], Count, Bad, Depth, ExpectedIRQL;
static void power(U64 Context, U64 A, U64 B) {
  (void)Context;
  (void)A;
  (void)B;
  Bad = 1;
}
static void record(U64 Context, U64 A, U64 B) {
  if (Count >= 16 || KeGetCurrentIrql() != ExpectedIRQL || B != A + 1) {
    Bad = 1;
    return;
  }
  Log[Count++] = (U32)((Context << 8) | A);
}
static void first(U64 Context, U64 A, U64 B) {
  record(Context, A, B);
  if (A == 1 && !Depth) {
    Depth = 1;
    ExNotifyCallback(Object, 11, 12);
    Depth = 0;
  }
}
static void second(U64 Context, U64 A, U64 B) { record(Context, A, B); }
static void string(US *S, U16 *Text, const char *Name) {
  U32 I = 0;
  for (; Name[I]; ++I)
    Text[I] = (U8)Name[I];
  Text[I] = 0;
  S->Length = (U16)(I * 2);
  S->MaximumLength = (U16)((I + 1) * 2);
  S->Buffer = (U64)Text;
}

static void blocking(U64 Context, U64 A, U64 B) {
  (void)Context;
  (void)A;
  (void)B;
  KeSetEvent(&Started, 0, 0);
  if (KeWaitForSingleObject(&Continue, 0, 0, 0, 0))
    Bad = 1;
}
static void notifier(void *Context) {
  (void)Context;
  ExNotifyCallback(Object, 55, 56);
  PsTerminateSystemThread(0);
}
static void unregistering(void *Context) {
  (void)Context;
  KeSetEvent(&WaiterStarted, 0, 0);
  ExUnregisterCallback(ConcurrentRegistration);
  UnregisterReturned = 1;
  PsTerminateSystemThread(0);
}
static void unload(void *Driver) {
  (void)Driver;
  ExUnregisterCallback(PowerRegistration);
  PowerRegistration = 0;
  ObfDereferenceObject(PowerObject);
  PowerObject = 0;
}
U32 DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  U16 Text[80];
  US Name;
  OA Attr = {sizeof(OA), 0, (U64)&Name, 0x40, 0, 0};
  string(&Name, Text, "\\Callback\\PowerState");
  if (ExCreateCallback(&PowerObject, &Attr, 0, 0))
    return 0xc0000001;
  PowerRegistration = ExRegisterCallback(PowerObject, power, 0);
  if (!PowerRegistration || Bad)
    return 0xc0000001;
  string(&Name, Text, "\\Callback\\NeverDDriverTest");
  if (ExCreateCallback(&Object, &Attr, 1, 1))
    return 0xc0000001;
  U64 ClosedName = 0x77777777;
  if (ExCreateCallback(&ClosedName, &Attr, 0, 1) != 0xc0000034 ||
      ClosedName != 0x77777777)
    return 0xc0000001;
  U64 A = ExRegisterCallback(Object, first, 0x11),
      B = ExRegisterCallback(Object, second, 0x22);
  if (!A || !B || Count)
    return 0xc0000001;
  ExNotifyCallback(Object, 1, 2);
  if (Bad || Count != 4 || Log[0] != 0x1101 || Log[1] != 0x110b ||
      Log[2] != 0x220b || Log[3] != 0x2201)
    return 0xc0000001;
  ExpectedIRQL = 2;
  U8 Old = KfRaiseIrql(2);
  ExNotifyCallback(Object, 21, 22);
  KeLowerIrql(Old);
  ExpectedIRQL = 0;
  if (Bad || Count != 6 || Log[4] != 0x1115 || Log[5] != 0x2215)
    return 0xc0000001;
  ExUnregisterCallback(B);
  ExNotifyCallback(Object, 31, 32);
  if (Bad || Count != 7 || Log[6] != 0x111f)
    return 0xc0000001;
  ExUnregisterCallback(A);
  ObfDereferenceObject(Object);
  Object = 0;
  U64 Missing = 0x77777777;
  if (ExCreateCallback(&Missing, &Attr, 0, 1) != 0xc0000034 ||
      Missing != 0x77777777)
    return 0xc0000001;
  if (ExCreateCallback(&Object, &Attr, 1, 0))
    return 0xc0000001;
  A = ExRegisterCallback(Object, first, 0x11);
  B = ExRegisterCallback(Object, second, 0x22);
  if (!A || B)
    return 0xc0000001;
  ExUnregisterCallback(A);
  ObfDereferenceObject(Object);
  Object = 0;

  if (ExCreateCallback(&Object, &Attr, 1, 1))
    return 0xc0000001;
  ConcurrentRegistration = ExRegisterCallback(Object, blocking, 0);
  if (!ConcurrentRegistration)
    return 0xc0000001;
  KeInitializeEvent(&Started, 0, 0);
  KeInitializeEvent(&WaiterStarted, 0, 0);
  KeInitializeEvent(&Continue, 0, 0);
  U64 Threads[2] = {0, 0}, ThreadObjects[2] = {0, 0};
  OA ThreadAttr = {sizeof(OA), 0, 0, 0x200, 0, 0};
  if (PsCreateSystemThread(&Threads[0], 0x1fffff, &ThreadAttr, 0, 0, notifier,
                           0) ||
      ObReferenceObjectByHandle(Threads[0], 0, 0, 0, &ThreadObjects[0], 0) ||
      KeWaitForSingleObject(&Started, 0, 0, 0, 0))
    return 0xc0000001;
  if (PsCreateSystemThread(&Threads[1], 0x1fffff, &ThreadAttr, 0, 0,
                           unregistering, 0) ||
      ObReferenceObjectByHandle(Threads[1], 0, 0, 0, &ThreadObjects[1], 0) ||
      KeWaitForSingleObject(&WaiterStarted, 0, 0, 0, 0))
    return 0xc0000001;
  if (UnregisterReturned)
    return 0xc0000001;
  KeSetEvent(&Continue, 0, 0);
  for (U32 I = 0; I < 2; ++I) {
    if (KeWaitForSingleObject((void *)ThreadObjects[I], 0, 0, 0, 0) ||
        ZwClose(Threads[I]))
      return 0xc0000001;
    ObfDereferenceObject(ThreadObjects[I]);
  }
  if (!UnregisterReturned || Bad)
    return 0xc0000001;
  ObfDereferenceObject(Object);
  Object = 0;
  *(U64 *)((U8 *)Driver + 0x68) = (U64)unload;
  return 0;
}
