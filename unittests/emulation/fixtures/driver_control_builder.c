// Original freestanding x64 WDM client of the nine-argument IOCTL builder.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int STATUS;
typedef struct {
  U8 Prefix[0x68];
  void (*Unload)(void *);
  void *Major[28];
} DRIVER;
typedef struct {
  U32 Status, Padding;
  U64 Information;
} IOSB;
#define FIELD(P, O, T) (*(T *)((U8 *)(P) + (O)))
__declspec(dllimport) STATUS IoCreateDevice(DRIVER *, U32, void *, U32, U32, U8,
                                            void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void *IoBuildDeviceIoControlRequest(U32, void *, void *,
                                                          U32, void *, U32, U8,
                                                          void *, IOSB *);
__declspec(dllimport) STATUS IofCallDriver(void *, void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) void KeInitializeEvent(void *, U32, U8);
__declspec(dllimport) U32 KeReadStateEvent(void *);
__declspec(dllimport) void KeClearEvent(void *);
__declspec(dllimport) STATUS KeWaitForSingleObject(void *, U32, U8, U8, void *);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(void *, U8, U32,
                                                         void *, U32, U32);
__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void IoMarkIrpPending(void *);
__declspec(dllimport) int DbgPrint(const char *, ...);
static void *Device, *Event, *Input, *Output, *Child, *Work;
static IOSB Block;
static U32 Method, Internal, Failures, Mode, Completions;
static void Check(U32 Good) {
  if (!Good) {
    ++Failures;
    DbgPrint("IOCTL builder invalid");
  }
}
static STATUS Completion(void *D, void *Irp, void *Context) {
  Check(!D && Irp == Child && Context == &Block && Block.Status == 0x77777777);
  ++Completions;
  return Mode == 2 ? (STATUS)0xc0000016 : 0;
}
static void CompleteChild(void *Irp) {
  FIELD(Irp, 0x30, STATUS) = 0;
  FIELD(Irp, 0x38, U64) = 8;
  IofCompleteRequest(Irp, 0);
}
static void Worker(void *D, void *Context) {
  Check(D == Device && Context == Child);
  IoFreeWorkItem(Work);
  Work = 0;
  CompleteChild(Context);
}
static STATUS Dispatch(void *D, void *Irp) {
  U8 *Stack = FIELD(Irp, 0xb8, U8 *);
  void *System = FIELD(Irp, 0x18, void *), *Mdl = FIELD(Irp, 8, void *),
       *Target;
  Check(D == Device && Irp == Child && Stack[0] == (Internal ? 15 : 14) &&
        FIELD(Stack, 8, U32) == 8 && FIELD(Stack, 16, U32) == 4 &&
        FIELD(Stack, 24, U32) == (0x222000 | Method) &&
        FIELD(Irp, 0x40, U8) == 0 && FIELD(Irp, 0x48, void *) == &Block &&
        FIELD(Irp, 0x98, void *) != 0);
  if (Method == 0) {
    Check(System && !Mdl && FIELD(System, 0, U32) == 0x04030201);
    Target = System;
  } else if (Method == 3) {
    Check(!System && !Mdl && FIELD(Stack, 32, void *) == Input);
    Target = FIELD(Irp, 0x70, void *);
  } else {
    Check(System && Mdl && FIELD(System, 0, U32) == 0x04030201);
    Target = MmMapLockedPagesSpecifyCache(Mdl, 0, 1, 0, 0, 16);
  }
  Check(Target != 0);
  if (Method == 1)
    Check(FIELD(Target, 0, U64) == 0xaaaaaaaaaaaaaaaaULL);
  else
    FIELD(Target, 0, U64) = 0x8877665544332211ULL;
  if (Mode == 1) {
    Work = IoAllocateWorkItem(Device);
    Check(Work != 0);
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(Work, Worker, 1, Irp);
    return 0x103;
  }
  CompleteChild(Irp);
  return 0;
}
static void Unload(void *Driver) {
  (void)Driver;
  IoDeleteDevice(Device);
  ExFreePoolWithTag(Input, 0x636f6e74);
  ExFreePoolWithTag(Output, 0x636f6e74);
  ExFreePoolWithTag(Event, 0x636f6e74);
  DbgPrint("IOCTL builder unload passed");
}
STATUS DriverEntry(DRIVER *Driver, void *Registry) {
  (void)Registry;
  if (IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device))
    return (STATUS)0xc000000d;
  Driver->Major[14] = (void *)Dispatch;
  Driver->Major[15] = (void *)Dispatch;
  Driver->Unload = Unload;
  Input = ExAllocatePoolWithTag(0, 64, 0x636f6e74);
  Output = ExAllocatePoolWithTag(0, 64, 0x636f6e74);
  Event = ExAllocatePoolWithTag(0, 24, 0x636f6e74);
  if (!Input || !Output || !Event)
    return (STATUS)0xc000009a;
  KeInitializeEvent(Event, 0, 0);
  for (Mode = 0; Mode < 3; ++Mode)
    for (Internal = 0; Internal < 2; ++Internal)
      for (Method = 0; Method < 4; ++Method) {
        FIELD(Input, 0, U32) = 0x04030201;
        FIELD(Output, 0, U64) = 0xaaaaaaaaaaaaaaaaULL;
        Block.Status = 0x77777777;
        Block.Information = 0x7777777777777777ULL;
        KeClearEvent(Event);
        Child = IoBuildDeviceIoControlRequest(0x222000 | Method, Device, Input,
                                              4, Output, 8, (U8)Internal,
                                              Mode == 2 ? 0 : Event, &Block);
        if (!Child)
          return (STATUS)0xc000009a;
        if (Mode == 2) {
          U8 *Next = FIELD(Child, 0xb8, U8 *) - 0x48;
          FIELD(Next, 0x38, void *) = (void *)Completion;
          FIELD(Next, 0x40, void *) = &Block;
          Next[3] = 0xe0;
        }
        STATUS Sent = IofCallDriver(Device, Child);
        Check(Sent == (Mode == 1 ? 0x103 : 0));
        if (Mode == 1)
          Check(KeWaitForSingleObject(Event, 0, 0, 0, 0) == 0);
        if (Mode == 2) {
          Check(Block.Status == 0x77777777);
          IofCompleteRequest(Child, 0);
        }
        Check(Block.Status == 0 && Block.Information == 8 &&
              FIELD(Output, 0, U64) == (Method == 1 ? 0xaaaaaaaaaaaaaaaaULL
                                                    : 0x8877665544332211ULL));
        if (Mode != 2)
          Check(KeReadStateEvent(Event) == 1);
        Child = 0;
      }
  Check(Completions == 8);
  if (Failures)
    return (STATUS)0xc000000d;
  DbgPrint("IOCTL builder 24 requests passed");
  return 0;
}
