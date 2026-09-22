//===- driver_async.c - Original asynchronous WDM fixture -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Extend the software lifecycle fixture with deferred IOCTL completion.
///
//===----------------------------------------------------------------------===//

#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

#define NEVERD_ASYNC_CASE(Name, Code) enum { Name = Code };
#include "DriverAsyncCases.def"
#undef NEVERD_ASYNC_CASE
#define NEVERD_WDM_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/windows/KernelValues.def"
#include "../../../lib/emulation/windows/WindowsKernelLayout.def"
#undef NEVERD_WDM_VALUE

__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void IoMarkIrpPending(IRP *);
__declspec(dllimport) U8 KeGetCurrentIrql(void);

static void *WorkItem;
static U32 Mode, Runs;

static void Worker(void *Target, void *Context) {
  IRP *Request = Context;
  if (Mode == WorkerLoop)
    for (;;)
      __asm__ volatile("pause");
  if (Mode == CallbackFault)
    *(volatile U32 *)0x1234 = 1;
  if (Mode == RequeueCurrent && !Runs++) {
    IoQueueWorkItem(WorkItem, Worker, 1, Request);
    return;
  }
  IoFreeWorkItem(WorkItem);
  if (Mode == DeleteInWorker)
    IoDeleteDevice(Device);
  if (Mode == DoubleFree)
    IoFreeWorkItem(WorkItem);
  if (Mode == WorkerNoCompletion)
    return;
  NTSTATUS Status = 0;
  U64 Information = 0;
  if (Target != Device || KeGetCurrentIrql() != 0 || Mode == DeferredFailure)
    Status = (NTSTATUS)0xc000000dU;
  else if (Request->Stack->OutputLength < Request->Stack->InputLength)
    Status = (NTSTATUS)0xc0000023U;
  else {
    Information = Request->Stack->InputLength;
    for (U32 I = 0; I < Information; ++I)
      Request->SystemBuffer[I] ^= 0x5a;
  }
  Request->Status = Status;
  Request->Information = Information;
  IofCompleteRequest(Request, 0);
  if (Mode == CompletedAccess)
    *(volatile U32 *)&Request->Status = 0;
}

static NTSTATUS AsyncDispatch(void *Target, IRP *Request) {
  (void)Target;
  Mode = Request->Stack->ControlCode;
  Runs = 0;
  if (Mode != MissingMark) {
    if (Mode == DeferredSuccess)
      IoMarkIrpPending(Request);
    else
      Request->Stack->Control |= StackPendingReturned;
  }
  if (Mode == NoProducer)
    return StatusPending;
  if (Mode == EarlyCompletion || Mode == EarlyWrongReturn) {
    Request->Status = 0;
    Request->Information = 0;
    IofCompleteRequest(Request, 0);
    return Mode == EarlyCompletion ? StatusPending : 0;
  }
  WorkItem = IoAllocateWorkItem(Device);
  if (Mode == OpaqueWorkItem)
    *(volatile U8 *)WorkItem = 1;
  IoQueueWorkItem(WorkItem, Worker, 1, Request);
  if (Mode == DoubleQueue)
    IoQueueWorkItem(WorkItem, Worker, 1, Request);
  if (Mode == FreeQueued)
    IoFreeWorkItem(WorkItem);
  return Mode == MarkedSuccess ? 0 : StatusPending;
}

static void AsyncUnload(DRIVER_OBJECT *Driver) {
  if (Mode == DeleteInWorker)
    IoDeleteSymbolicLink(&LinkName);
  else
    Unload(Driver);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  NTSTATUS Status = BufferedDriverEntry(Driver, RegistryPath);
  if (Status >= 0) {
    Driver->MajorFunction[14] = (void *)AsyncDispatch;
    Driver->DriverUnload = (void *)AsyncUnload;
  }
  return Status;
}
