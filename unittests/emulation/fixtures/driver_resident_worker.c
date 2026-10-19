//===- driver_resident_worker.c - Resident WDM worker lifecycle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

typedef long long I64;
__declspec(dllimport) NTSTATUS PsCreateSystemThread(void **, U32, void *,
                                                    void *, void *,
                                                    void (*)(void *), void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(void *, U32, void *,
                                                         U32, void **, void *);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(U32, U8, const I64 *);
__declspec(dllimport) void PsTerminateSystemThread(U32);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, U32, U8,
                                                     const I64 *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) void IoMarkIrpPending(IRP *);
__declspec(dllimport) void *IoAllocateWorkItem(void *);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(void *, void *),
                                           U32, void *);

static volatile U32 StopWorker;
static IRP *volatile PendingRequest;
static void *WorkerHandle, *WorkerThread;
#ifdef RESIDENT_PACING
static volatile U32 WorkerTicks;
#endif

static void ResidentWorker(void *Context) {
  (void)Context;
  const I64 Interval = -10000;
  while (!StopWorker) {
#ifdef RESIDENT_PACING
    ++WorkerTicks;
#endif
#ifdef RESIDENT_RUNNABLE
    __asm__ volatile("pause");
#else
    IRP *Request = PendingRequest;
    if (Request) {
      PendingRequest = 0;
      for (U32 I = 0; I < 4; ++I)
        Request->SystemBuffer[I] = (U8)(0xa0 + I);
      Request->Information = 4;
      Request->Status = 0;
      IofCompleteRequest(Request, 0);
    }
    KeDelayExecutionThread(0, 0, &Interval);
#endif
  }
  PsTerminateSystemThread(0);
}

#ifdef RESIDENT_WORKITEM
static void ResidentWorkItem(void *Target, void *Context) {
  (void)Target;
  ResidentWorker(Context);
}
#endif

static NTSTATUS ResidentDispatch(void *Target, IRP *Request) {
#ifdef RESIDENT_PACING
  if (Request->Stack->ControlCode == 0x222024U) {
    if (Request->Stack->OutputLength < 4)
      return (NTSTATUS)0xc000000dU;
    *(U32 *)Request->SystemBuffer = WorkerTicks;
    Request->Information = 4;
    Request->Status = 0;
    IofCompleteRequest(Request, 0);
    return 0;
  }
#endif
  if (Request->Stack->ControlCode != 0x222020U)
    return Dispatch(Target, Request);
  if (Target != Device || Request->Stack->File->FsContext != Device ||
      Request->Stack->OutputLength < 4 || PendingRequest) {
    Request->Status = (NTSTATUS)0xc000000dU;
    Request->Information = 0;
    IofCompleteRequest(Request, 0);
    return Request->Status;
  }
  IoMarkIrpPending(Request);
  PendingRequest = Request;
  return 0x103;
}

static void ResidentUnload(DRIVER_OBJECT *Driver) {
#ifndef RESIDENT_LEAK
  StopWorker = 1;
  if (KeWaitForSingleObject(WorkerThread, 0, 0, 0, 0) || ZwClose(WorkerHandle))
    return;
  ObfDereferenceObject(WorkerThread);
#endif
  Unload(Driver);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *Path) {
  NTSTATUS Status = BufferedDriverEntry(Driver, Path);
  if (Status)
    return Status;
#ifdef RESIDENT_WORKITEM
  void *Work = IoAllocateWorkItem(Device);
  if (!Work)
    return (NTSTATUS)0xc000009aU;
  IoQueueWorkItem(Work, ResidentWorkItem, 1, 0);
#else
  Status = PsCreateSystemThread(&WorkerHandle, 0x10000000, 0, 0, 0,
                                ResidentWorker, 0);
  if (Status) {
    Unload(Driver);
    return Status;
  }
  Status = ObReferenceObjectByHandle(WorkerHandle, 0, 0, 0, &WorkerThread, 0);
  if (Status)
    return Status;
#endif
  Driver->MajorFunction[14] = (void *)ResidentDispatch;
  Driver->DriverUnload = (void *)ResidentUnload;
  return 0;
}
