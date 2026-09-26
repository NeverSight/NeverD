//===- driver_context_limits.c - Callback context fixture ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original x64 WDM code exercising actual scheduled IRQL and stack limits.
///
//===----------------------------------------------------------------------===//

#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

#define NEVERD_CONTEXT_LIMIT_FAILURE(Name, Code, Diagnostic, API)              \
  enum { Name = Code };
#define NEVERD_CONTEXT_LIMIT_SUCCESS(Name, Code) enum { Name = Code };
#define NEVERD_CONTEXT_LIMIT_STACK(Name, Code) enum { Name = Code };
#define NEVERD_CONTEXT_LIMIT_STORAGE(Name, Code) enum { Name = Code };
#define NEVERD_CONTEXT_LIMIT_ENTRY(Name, Marker, Service)                      \
  enum { Name = Marker };
#include "DriverContextLimitCases.def"
#undef NEVERD_CONTEXT_LIMIT_ENTRY
#undef NEVERD_CONTEXT_LIMIT_STORAGE
#undef NEVERD_CONTEXT_LIMIT_STACK
#undef NEVERD_CONTEXT_LIMIT_SUCCESS
#undef NEVERD_CONTEXT_LIMIT_FAILURE

#define NEVERD_WDM_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/windows/KernelValues.def"
#include "../../../lib/emulation/windows/WindowsKernelLayout.def"
#undef NEVERD_WDM_VALUE
#define NEVERD_KERNEL_POOL_FLAG(Name, Value) enum { PoolFlag##Name = Value };
#include "../../../lib/emulation/windows/KernelPoolFlags.def"
#undef NEVERD_KERNEL_POOL_FLAG
#define NEVERD_KERNEL_DISPATCHER_VALUE(Name, Value)                            \
  enum { Context##Name = Value };
#include "../../../lib/emulation/windows/KernelDispatcherValues.def"
#undef NEVERD_KERNEL_DISPATCHER_VALUE

// WDK dispatcher storage is opaque. Win64 objects have eight-byte alignment.
typedef struct {
  U64 Reserved[ContextDpcSize / sizeof(U64)];
} CONTEXT_DPC;
typedef struct {
  U64 Reserved[ContextTimerSize / sizeof(U64)];
} CONTEXT_TIMER;
typedef struct {
  U64 Reserved[ContextEventSize / sizeof(U64)];
} CONTEXT_EVENT;
typedef union {
  long long QuadPart;
  U64 Unsigned;
} LARGE_INTEGER;
typedef struct {
  U32 Length;
  void *RootDirectory;
  UNICODE_STRING *ObjectName;
  U32 Attributes;
  void *SecurityDescriptor;
  void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
_Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48, "x64 OBJECT_ATTRIBUTES");
_Static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, ObjectName) == 16,
               "x64 OBJECT_ATTRIBUTES name pointer");
_Static_assert(_Alignof(CONTEXT_DPC) == 8, "x64 dispatcher alignment");
_Static_assert(sizeof(LARGE_INTEGER) == 8, "x64 LARGE_INTEGER by-value ABI");

__declspec(dllimport) void
KeInitializeDpc(CONTEXT_DPC *, void (*)(CONTEXT_DPC *, void *, void *, void *),
                void *);
__declspec(dllimport) U8 KeInsertQueueDpc(CONTEXT_DPC *, void *, void *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void KeInitializeTimer(CONTEXT_TIMER *);
__declspec(dllimport) U8 KeSetTimer(CONTEXT_TIMER *, LARGE_INTEGER,
                                    CONTEXT_DPC *);
__declspec(dllimport) void KeInitializeEvent(CONTEXT_EVENT *, U32, U8);
__declspec(dllimport) int KeSetEvent(CONTEXT_EVENT *, int, U8);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, signed char,
                                                     U8, LARGE_INTEGER *);
__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void IoMarkIrpPending(IRP *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void *ExAllocatePool2(U64, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) NTSTATUS ZwOpenKey(void **, U32, OBJECT_ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) void *MmMapLockedPagesSpecifyCache(void *, U32, U32,
                                                         void *, U8, U32);
__declspec(dllimport) void MmUnmapLockedPages(void *, void *);

enum { ContextTag = 0x4354584c, ContextBufferSize = 32, DelayedQueue = 1 };
static CONTEXT_DPC Dpc;
static CONTEXT_DPC WaitedStorage;
static CONTEXT_EVENT Event;
static UNICODE_STRING KeyName, DebugText;
static U32 Mode;
static volatile U8 *PagedBytes;
static void *WaitItem, *EscapeItem;

static void StorageDeferred(CONTEXT_DPC *Object, void *Context, void *First,
                            void *Second) {
  (void)Object;
  (void)Context;
  (void)First;
  (void)Second;
  DbgPrint("context released storage reached forbidden DPC continuation\n");
}

static void Complete(IRP *Request, int Valid) {
  Request->Status = Valid ? StatusSuccess : (NTSTATUS)0xc000000dU;
  Request->Information = Valid ? 4 : 0;
  if (Valid) {
    Request->SystemBuffer[0] = ContextDispatchLevel;
    Request->SystemBuffer[1] = 'O';
    Request->SystemBuffer[2] = 'K';
    Request->SystemBuffer[3] = 0xa5;
  }
  IofCompleteRequest(Request, 0);
}

static void Deferred(CONTEXT_DPC *Object, void *Context, void *First,
                     void *Second) {
  (void)First;
  (void)Second;
  const U8 IRQL = KeGetCurrentIrql();
  int Valid = Object == &Dpc && IRQL == ContextDispatchLevel;
  if (Mode == RegistryAtDpc) {
    OBJECT_ATTRIBUTES Attributes = {
        sizeof(Attributes), 0, &KeyName, 0x240, 0, 0};
    void *Handle = 0;
    NTSTATUS Status = ZwOpenKey(&Handle, 0x20019, &Attributes);
    if (Status >= 0)
      ZwClose(Handle);
    Valid = 0;
  } else if (Mode == LegacyPagedAtDpc || Mode == ModernPagedAtDpc) {
    void *Buffer =
        Mode == LegacyPagedAtDpc
            ? ExAllocatePoolWithTag(PoolPaged, ContextBufferSize, ContextTag)
            : ExAllocatePool2(PoolFlagPaged, ContextBufferSize, ContextTag);
    if (Buffer)
      ExFreePoolWithTag(Buffer, ContextTag);
    Valid = 0;
  } else if (Mode == NonpagedAtDpc) {
    volatile U8 *Legacy =
        ExAllocatePoolWithTag(PoolNX, ContextBufferSize, ContextTag);
    volatile U8 *Modern =
        ExAllocatePool2(PoolFlagNonPaged, ContextBufferSize, ContextTag);
    if (Legacy && Modern) {
      Legacy[0] = 0x5a;
      Modern[0] = Legacy[0] ^ 0xff;
      Valid &= Modern[0] == 0xa5;
    } else
      Valid = 0;
    if (Modern)
      ExFreePoolWithTag((void *)Modern, ContextTag);
    if (Legacy)
      ExFreePoolWithTag((void *)Legacy, ContextTag);
  } else if (Mode == PagedReadAtDpc) {
    Valid &= PagedBytes[0] == 0xa5;
  } else if (Mode == PagedWriteAtDpc) {
    PagedBytes[0] = 0x5a;
  } else if (Mode == UnicodePrintAtDpc) {
    DbgPrint("context Unicode=%wZ\n", &DebugText);
  } else if (Mode == AnsiPrintAtDpc) {
    Valid &= DbgPrint("context ANSI IRQL=%u\n", (U32)IRQL) == 0;
  } else {
    Valid = 0;
  }
  if (PagedBytes) {
    ExFreePoolWithTag((void *)PagedBytes, ContextTag);
    PagedBytes = 0;
  }
  Complete((IRP *)Context, Valid);
}

static void WaitWorker(void *Target, void *Context) {
  volatile U64 Canary[4];
  Canary[0] = 0x1122334455667788ULL;
  Canary[1] = 0x8877665544332211ULL;
  Canary[2] = (U64)&Canary[0];
  Canary[3] = Canary[0] ^ Canary[1];
  int Valid = Target == Device && KeGetCurrentIrql() == 0;
  void *WaitObject =
      Mode == ReplaceWaitedEvent ? (void *)&WaitedStorage : (void *)&Event;
  Valid &= KeWaitForSingleObject(WaitObject, ExecutiveWaitReason, KernelMode, 0,
                                 0) == StatusSuccess;
  Valid &= Canary[0] == 0x1122334455667788ULL;
  Valid &= Canary[1] == 0x8877665544332211ULL;
  Valid &= Canary[2] == (U64)&Canary[0];
  Valid &= Canary[3] == (Canary[0] ^ Canary[1]);
  IoFreeWorkItem(WaitItem);
  Complete((IRP *)Context, Valid);
}

static void EscapeWorker(void *Target, void *Context) {
  (void)Target;
  (void)Context;
  // A single allocation can jump over the guard page into another callback's
  // stack. The instruction hook must reject the new RSP before the write.
  // Keep both stack changes in one asm block, with balanced compiler-visible
  // entry/exit state; this intentionally avoids __chkstk import dependencies.
  __asm__ volatile("subq $0x11000, %%rsp\n\t"
                   "movq $0x1234, (%%rsp)\n\t"
                   "addq $0x11000, %%rsp"
                   :
                   :
                   : "memory");
  DbgPrint("context stack escape reached forbidden continuation\n");
  KeSetEvent(&Event, 0, 0);
  IoFreeWorkItem(EscapeItem);
}

static void ReplaceWorker(void *Target, void *Context) {
  (void)Target;
  (void)Context;
  // The same allocation can fit either type. Replacement must fail because
  // another frame waits on the event, before changing its type or bytes.
  KeInitializeDpc(&WaitedStorage, StorageDeferred, 0);
  DbgPrint("context waited event reached forbidden replacement continuation\n");
  IoFreeWorkItem(EscapeItem);
}

static NTSTATUS ContextDispatch(void *Target, IRP *Request) {
  (void)Target;
  Mode = Request->Stack->ControlCode;
  PagedBytes = 0;
  if (Mode == MdlUnmapQueuedDpc) {
    void *Mdl = *(void **)((U8 *)Request + IRPMdlOffset);
    CONTEXT_DPC *Mapped = MmMapLockedPagesSpecifyCache(
        Mdl, KernelMode, MmCached, 0, 0, NormalPagePriority);
    KeInitializeDpc(Mapped, StorageDeferred, 0);
    KeInsertQueueDpc(Mapped, 0, 0);
    MmUnmapLockedPages(Mapped, Mdl);
    Request->Status = StatusSuccess;
    Request->Information = 0;
    IofCompleteRequest(Request, 0);
    return StatusSuccess;
  }
  if (Mode == PagedReadAtDpc || Mode == PagedWriteAtDpc) {
    PagedBytes =
        ExAllocatePoolWithTag(PoolPaged, ContextBufferSize, ContextTag);
    if (!PagedBytes) {
      Complete(Request, 0);
      return (NTSTATUS)0xc000009aU;
    }
    PagedBytes[0] = 0xa5;
  }
  IoMarkIrpPending(Request);
  if (Mode == WorkerStackEscape || Mode == ReplaceWaitedEvent) {
    CONTEXT_EVENT *WaitObject =
        Mode == ReplaceWaitedEvent ? (CONTEXT_EVENT *)&WaitedStorage : &Event;
    KeInitializeEvent(WaitObject, ContextNotificationObject, 0);
    WaitItem = IoAllocateWorkItem(Device);
    EscapeItem = IoAllocateWorkItem(Device);
    IoQueueWorkItem(WaitItem, WaitWorker, DelayedQueue, Request);
    IoQueueWorkItem(EscapeItem,
                    Mode == ReplaceWaitedEvent ? ReplaceWorker : EscapeWorker,
                    DelayedQueue, 0);
  } else {
    KeInitializeDpc(&Dpc, Deferred, Request);
    KeInsertQueueDpc(&Dpc, 0, 0);
  }
  return StatusPending;
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  if (RegistryPath->Length &&
      RegistryPath->Buffer[RegistryPath->Length / sizeof(U16) - 1] ==
          RegistryPathQueuedDpc) {
    if (RegistryPath->MaximumLength < sizeof(CONTEXT_DPC))
      return (NTSTATUS)0xc000000dU;
    CONTEXT_DPC *Borrowed = (CONTEXT_DPC *)(void *)RegistryPath->Buffer;
    KeInitializeDpc(Borrowed, StorageDeferred, 0);
    KeInsertQueueDpc(Borrowed, 0, 0);
    return StatusSuccess;
  }
  if (RegistryPath->Length &&
      RegistryPath->Buffer[RegistryPath->Length / sizeof(U16) - 1] ==
          DeleteDeviceArmedTimer) {
    // No open file or work reference can postpone this deletion. The active
    // timer embedded in the extension must stop release of the device storage.
    NTSTATUS Status =
        IoCreateDevice(Driver, sizeof(CONTEXT_TIMER), 0, 0x22, 0, 0, &Device);
    if (Status < 0)
      return Status;
    CONTEXT_TIMER *Timer =
        *(CONTEXT_TIMER **)((U8 *)Device + DeviceExtensionOffset);
    LARGE_INTEGER Due = {-100000};
    KeInitializeTimer(Timer);
    KeSetTimer(Timer, Due, 0);
    IoDeleteDevice(Device);
    return (NTSTATUS)0xc000000dU;
  }
  NTSTATUS Status = BufferedDriverEntry(Driver, RegistryPath);
  if (Status < 0)
    return Status;
  RtlInitUnicodeString(&KeyName, L"\\Registry\\Machine\\System\\CurrentControlS"
                                 L"et\\Services\\NeverDDriver");
  RtlInitUnicodeString(&DebugText, L"context");
  Driver->MajorFunction[14] = (void *)ContextDispatch;
  return Status;
}
