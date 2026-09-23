//===- driver_kmdf_control.c - Genuine WDK control-device fixture ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original KMDF code compiled against unmodified WDK 1.33 headers and linked
/// through the genuine wdfdriverentry.lib FxDriverEntry entry point. Microsoft
/// headers and libraries remain external, optional validation dependencies.
///
/// The final service-name character selects buffered READ/WRITE (B, default),
/// direct READ/WRITE (D), or buffered READ/WRITE with deferred IOCTL completion
/// (W). C observes request cleanup, child destruction and retained context.
/// X completes from a cancel callback; H delegates cancel completion to a
/// worker while the cancel callback waits; U unmarks before a delayed worker;
/// N deliberately completes a still-cancelable request. Buffered mode B polls
/// cancellation before processing, including the CLI's default service name.
/// L uses legacy MarkCancelable, including synchronous cancellation before the
/// API returns. M checks request metadata and buffered WDM MDL aliases.
/// I preprocesses each transfer in EvtIoInCallerContext before enqueueing it.
/// J deliberately returns from that callback without enqueue or completion.
/// K completes the transfer in the caller-context callback without enqueueing.
/// Q deliberately supplies an invalid queue configuration size. CREATE,
/// CLEANUP and CLOSE use the default framework file package without callbacks.
///
/// READ returns byte 0x60 + (offset & 0x1f). WRITE accepts bytes matching
/// 0x40 + (offset & 0x1f), completing STATUS_DATA_ERROR on a mismatch. Buffered
/// IOCTL 0x222000 accepts nonempty input and an output buffer at least four
/// bytes larger; it returns 'K', 'M', 'D', the selected mode, then input XOR
/// 0x5a. Input/output logical lengths must remain distinct despite buffer
/// aliasing.
///
//===----------------------------------------------------------------------===//

#include <ntifs.h>
#include <wdf.h>

#define ABI_OFFSET(Type, Member, Offset)                                       \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset,                   \
                 #Type "." #Member " x64 ABI")
#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")

#define IOCTL_NEVERD_KMDF_TRANSFORM                                            \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum {
  TransformPrefixLength = 4,
  TransformMask = 0x5a,
  ReadPatternBase = 0x60,
  WritePatternBase = 0x40,
  PatternIndexMask = 0x1f
};

_Static_assert(sizeof(void *) == 8, "x64 fixture");
_Static_assert(IOCTL_NEVERD_KMDF_TRANSFORM == 0x222000, "IOCTL ABI");
_Static_assert(sizeof(WDF_IO_QUEUE_CONFIG) == 96, "queue config ABI");
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, DispatchType, 4);
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, PowerManaged, 8);
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, AllowZeroLengthRequests, 12);
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, DefaultQueue, 13);
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, EvtIoRead, 24);
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, EvtIoWrite, 32);
ABI_OFFSET(WDF_IO_QUEUE_CONFIG, EvtIoDeviceControl, 40);
_Static_assert(sizeof(WDF_REQUEST_PARAMETERS) == 40, "request parameters ABI");
ABI_OFFSET(WDF_REQUEST_PARAMETERS, Type, 4);
ABI_OFFSET(WDF_REQUEST_PARAMETERS, Parameters.Read.Length, 8);
ABI_OFFSET(WDF_REQUEST_PARAMETERS, Parameters.Write.Length, 8);
ABI_OFFSET(WDF_REQUEST_PARAMETERS,
           Parameters.DeviceIoControl.OutputBufferLength, 8);
ABI_OFFSET(WDF_REQUEST_PARAMETERS, Parameters.DeviceIoControl.InputBufferLength,
           16);
ABI_OFFSET(WDF_REQUEST_PARAMETERS, Parameters.DeviceIoControl.IoControlCode,
           24);
_Static_assert(WdfDeviceIoBuffered == 2 && WdfDeviceIoDirect == 3,
               "I/O type ABI");
_Static_assert(WdfIoQueueDispatchSequential == 1, "queue dispatch ABI");
_Static_assert(WdfExecutionLevelPassive == 2, "passive execution ABI");
_Static_assert(WdfSynchronizationScopeNone == 4, "synchronization ABI");
ABI_SLOT(WdfControlDeviceInitAllocate, 25);
ABI_SLOT(WdfControlFinishInitializing, 27);
ABI_SLOT(WdfDeviceWdmGetDeviceObject, 31);
ABI_SLOT(WdfDeviceInitFree, 54);
ABI_SLOT(WdfDeviceInitSetIoType, 61);
ABI_SLOT(WdfDeviceInitSetIoInCallerContextCallback, 74);
ABI_SLOT(WdfDeviceInitAssignName, 67);
ABI_SLOT(WdfDeviceCreate, 75);
ABI_SLOT(WdfDeviceCreateSymbolicLink, 80);
ABI_SLOT(WdfDeviceGetDefaultQueue, 92);
ABI_SLOT(WdfDriverCreate, 116);
ABI_SLOT(WdfIoQueueCreate, 152);
ABI_SLOT(WdfDeviceEnqueueRequest, 91);
ABI_SLOT(WdfObjectAllocateContext, 203);
ABI_SLOT(WdfObjectReferenceActual, 205);
ABI_SLOT(WdfObjectDereferenceActual, 206);
ABI_SLOT(WdfObjectCreate, 207);
ABI_SLOT(WdfObjectDelete, 208);
ABI_SLOT(WdfRequestMarkCancelable, 255);
ABI_SLOT(WdfRequestUnmarkCancelable, 256);
ABI_SLOT(WdfRequestIsCanceled, 257);
ABI_SLOT(WdfRequestComplete, 263);
ABI_SLOT(WdfRequestCompleteWithInformation, 265);
ABI_SLOT(WdfRequestGetParameters, 266);
ABI_SLOT(WdfRequestRetrieveInputBuffer, 269);
ABI_SLOT(WdfRequestRetrieveOutputBuffer, 270);
ABI_SLOT(WdfRequestRetrieveInputWdmMdl, 271);
ABI_SLOT(WdfRequestRetrieveOutputWdmMdl, 272);
ABI_SLOT(WdfRequestSetInformation, 275);
ABI_SLOT(WdfRequestGetInformation, 276);
ABI_SLOT(WdfRequestGetFileObject, 277);
ABI_SLOT(WdfRequestGetIoQueue, 282);
ABI_SLOT(WdfRequestWdmGetIrp, 285);
ABI_SLOT(WdfRequestMarkCancelableEx, 393);

typedef struct {
  WDFREQUEST Request;
  PIO_WORKITEM Item;
  size_t OutputLength;
  size_t InputLength;
} DEFERRED_IOCTL_CONTEXT;

typedef struct {
  PUCHAR Buffer;
  PIRP Irp;
  size_t Length;
  ULONG Checksum;
  ULONG Phase;
  ULONG Cookie;
  BOOLEAN Armed;
} CLEANUP_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CLEANUP_REQUEST_CONTEXT, CleanupContext);

typedef struct {
  CLEANUP_REQUEST_CONTEXT *RequestContext;
} CLEANUP_CHILD_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CLEANUP_CHILD_CONTEXT, CleanupChildContext);

typedef struct {
  ULONG Cookie;
  ULONG Phase;
  UCHAR Mode;
  BOOLEAN CallbackActive;
  BOOLEAN DeferredDestroy;
  KEVENT Completed;
} CANCEL_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CANCEL_REQUEST_CONTEXT, CancelContext);

static WDFDEVICE CreatedDevice;
static WDFQUEUE DefaultQueue;
static UCHAR TransferMode;
static volatile ULONG CallerRequestCount;
static DEFERRED_IOCTL_CONTEXT Deferred;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("KMDF control: failure %lu\n", Code);
  return Condition;
}

static BOOLEAN CheckQueue(WDFQUEUE Queue, ULONG Code) {
  return Check(Queue == DefaultQueue && KeGetCurrentIrql() == PASSIVE_LEVEL,
               Code);
}

static BOOLEAN CheckRetiredRequestAccessors(WDFREQUEST Request, ULONG Code) {
  PMDL InputMdl = (PMDL)(ULONG_PTR)1;
  PMDL OutputMdl = (PMDL)(ULONG_PTR)1;
  NTSTATUS InputStatus = WdfRequestRetrieveInputWdmMdl(Request, &InputMdl);
  NTSTATUS OutputStatus = WdfRequestRetrieveOutputWdmMdl(Request, &OutputMdl);
  return Check(WdfRequestGetIoQueue(Request) == NULL &&
                   WdfRequestGetInformation(Request) == 0 &&
                   InputStatus == STATUS_INTERNAL_ERROR && InputMdl == NULL &&
                   OutputStatus == STATUS_INTERNAL_ERROR && OutputMdl == NULL,
               Code);
}

static PIRP CheckedRequestIrp(WDFREQUEST Request, UCHAR Major) {
  PIRP Irp = WdfRequestWdmGetIrp(Request);
  if (!Check(Irp != NULL && WdfRequestGetIoQueue(Request) == DefaultQueue &&
                 WdfRequestGetFileObject(Request) == NULL &&
                 IoGetCurrentIrpStackLocation(Irp)->MajorFunction == Major &&
                 IoGetCurrentIrpStackLocation(Irp)->FileObject != NULL,
             100))
    return NULL;
  return Irp;
}

static BOOLEAN CheckBufferedIoctlMdl(WDFREQUEST Request, PUCHAR Buffer,
                                     size_t InputLength) {
  PIRP Irp = CheckedRequestIrp(Request, IRP_MJ_DEVICE_CONTROL);
  PMDL InputMdl = NULL;
  PMDL OutputMdl = NULL;
  NTSTATUS InputStatus = WdfRequestRetrieveInputWdmMdl(Request, &InputMdl);
  NTSTATUS OutputStatus = WdfRequestRetrieveOutputWdmMdl(Request, &OutputMdl);
  // A buffered request caches one descriptor on first retrieval. The larger
  // output capacity must not silently replace that input-sized descriptor.
  return Check(
      Irp != NULL && Irp->MdlAddress == NULL && InputStatus == STATUS_SUCCESS &&
          OutputStatus == STATUS_SUCCESS && InputMdl != NULL &&
          OutputMdl == InputMdl && MmGetMdlByteCount(InputMdl) == InputLength &&
          MmGetSystemAddressForMdlSafe(InputMdl, NormalPagePriority) ==
              Buffer &&
          Irp->AssociatedIrp.SystemBuffer == Buffer,
      101);
}

static BOOLEAN CheckTransferMdl(WDFREQUEST Request, PUCHAR Buffer,
                                size_t Length, BOOLEAN WriteRequest) {
  PIRP Irp =
      CheckedRequestIrp(Request, WriteRequest ? IRP_MJ_WRITE : IRP_MJ_READ);
  PMDL Mdl = NULL;
  NTSTATUS Status = WriteRequest
                        ? WdfRequestRetrieveInputWdmMdl(Request, &Mdl)
                        : WdfRequestRetrieveOutputWdmMdl(Request, &Mdl);
  return Check(Irp != NULL && Status == STATUS_SUCCESS && Mdl != NULL &&
                   MmGetMdlByteCount(Mdl) == Length &&
                   MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority) ==
                       Buffer &&
                   (TransferMode == 'D' ? Irp->MdlAddress == Mdl
                                        : Irp->MdlAddress == NULL),
               102);
}

static void CompleteWithMetadata(WDFREQUEST Request, size_t Length) {
  PIRP Irp = WdfRequestWdmGetIrp(Request);
  const ULONG_PTR First = 0x123456789abcdef0ULL;
  const ULONG_PTR Second = 0xfedcba9876543210ULL;
  WdfRequestSetInformation(Request, First);
  if (!Check(Irp != NULL && WdfRequestGetInformation(Request) == First &&
                 Irp->IoStatus.Information == First,
             103)) {
    WdfRequestCompleteWithInformation(Request, STATUS_UNSUCCESSFUL, 0);
    return;
  }
  Irp->IoStatus.Information = Second;
  if (!Check(WdfRequestGetInformation(Request) == Second, 104)) {
    WdfRequestCompleteWithInformation(Request, STATUS_UNSUCCESSFUL, 0);
    return;
  }
  WdfRequestSetInformation(Request, Length);
  if (!Check(WdfRequestGetInformation(Request) == Length &&
                 Irp->IoStatus.Information == Length,
             105)) {
    WdfRequestCompleteWithInformation(Request, STATUS_UNSUCCESSFUL, 0);
    return;
  }
  DbgPrint("KMDF control: %c metadata checked\n", TransferMode);
  // No explicit Information argument: completion must use the shared IRP's
  // current value, including mutations made through either public interface.
  WdfRequestComplete(Request, STATUS_SUCCESS);
}

static ULONG BufferChecksum(volatile UCHAR *Buffer, size_t Length) {
  ULONG Value = 5381;
  for (size_t Index = 0; Index != Length; ++Index)
    Value = (Value * 33) ^ Buffer[Index];
  return Value;
}

static BOOLEAN CheckSavedOutput(CLEANUP_REQUEST_CONTEXT *Context, ULONG Code) {
  volatile UCHAR *Buffer = Context->Buffer;
  return Check(KeGetCurrentIrql() == PASSIVE_LEVEL && Buffer != NULL &&
                   Context->Length >= TransformPrefixLength &&
                   Buffer[0] == 'K' && Buffer[1] == 'M' && Buffer[2] == 'D' &&
                   Buffer[3] == 'C' &&
                   BufferChecksum(Buffer, Context->Length) == Context->Checksum,
               Code);
}

static void RequestCleanup(WDFOBJECT Object) {
  CLEANUP_REQUEST_CONTEXT *Context = CleanupContext(Object);
  if (!Context->Armed)
    return;
  if (Check(Context->Cookie == 0xc0de3233 && Context->Phase == 0, 70) &&
      CheckSavedOutput(Context, 71) &&
      CheckRetiredRequestAccessors((WDFREQUEST)Object, 106) &&
      Check(Context->Irp != NULL &&
                Context->Irp->IoStatus.Information == Context->Length - 1,
            108)) {
    // CompleteWithInformation publishes its value before EarlyDispose. The
    // saved WDM escape remains live here and is the final information source.
    Context->Irp->IoStatus.Information = Context->Length;
    DbgPrint("KMDF control: C cleanup information checked\n");
    Context->Phase = 1;
    DbgPrint("KMDF control: C request cleanup\n");
  }
}

static void RequestChildDestroy(WDFOBJECT Object) {
  CLEANUP_REQUEST_CONTEXT *Context =
      CleanupChildContext(Object)->RequestContext;
  if (Check(Context != NULL && Context->Armed &&
                Context->Cookie == 0xc0de3233 && Context->Phase == 1,
            72) &&
      CheckSavedOutput(Context, 73)) {
    Context->Phase = 2;
    DbgPrint("KMDF control: C child destroy\n");
  }
}

static void RequestDestroy(WDFOBJECT Object) {
  CLEANUP_REQUEST_CONTEXT *Context = CleanupContext(Object);
  if (!Context->Armed)
    return;
  // The reference retained this context, not the already completed WDM IRP or
  // its buffers. This callback deliberately never reads Context->Buffer.
  if (Check(KeGetCurrentIrql() == PASSIVE_LEVEL &&
                Context->Cookie == 0xc0de3233 && Context->Phase == 3,
            74))
    DbgPrint("KMDF control: C request destroy\n");
}

static void CompleteObservedTransform(WDFREQUEST Request, PUCHAR Buffer,
                                      size_t Length) {
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDFOBJECT Child = NULL;
  CLEANUP_REQUEST_CONTEXT *Context = NULL;
  NTSTATUS Status;

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, CLEANUP_REQUEST_CONTEXT);
  Attributes.EvtCleanupCallback = RequestCleanup;
  Attributes.EvtDestroyCallback = RequestDestroy;
  Status = WdfObjectAllocateContext(Request, &Attributes, (PVOID *)&Context);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  Context->Buffer = Buffer;
  Context->Irp = WdfRequestWdmGetIrp(Request);
  Context->Length = Length;
  Context->Checksum = BufferChecksum(Buffer, Length);
  Context->Cookie = 0xc0de3233;

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, CLEANUP_CHILD_CONTEXT);
  Attributes.ParentObject = Request;
  Attributes.EvtDestroyCallback = RequestChildDestroy;
  Status = WdfObjectCreate(&Attributes, &Child);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  CleanupChildContext(Child)->RequestContext = Context;
  Context->Armed = TRUE;
  WdfObjectReference(Request);
  WdfRequestSetInformation(Request, 1);
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length - 1);
  // Only the context is inspected after completion; no request accessor or
  // saved buffer dereference may extend the underlying IRP's lifetime.
  if (Check(CleanupContext(Request) == Context &&
                Context->Cookie == 0xc0de3233 && Context->Phase == 2,
            75) &&
      CheckRetiredRequestAccessors(Request, 107)) {
    Context->Phase = 3;
    DbgPrint("KMDF control: C retained context\n");
  }
  WdfObjectDereference(Request);
}

static void TransformRequest(WDFREQUEST Request, size_t OutputLength,
                             size_t InputLength) {
  WDF_REQUEST_PARAMETERS Parameters;
  PUCHAR Input = NULL;
  PUCHAR Output = NULL;
  size_t RetrievedInputLength = 0;
  size_t RetrievedOutputLength = 0;
  NTSTATUS Status;

  WDF_REQUEST_PARAMETERS_INIT(&Parameters);
  WdfRequestGetParameters(Request, &Parameters);
  if (!Check(KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 Parameters.Type == WdfRequestTypeDeviceControl &&
                 Parameters.Parameters.DeviceIoControl.OutputBufferLength ==
                     OutputLength &&
                 Parameters.Parameters.DeviceIoControl.InputBufferLength ==
                     InputLength &&
                 Parameters.Parameters.DeviceIoControl.IoControlCode ==
                     IOCTL_NEVERD_KMDF_TRANSFORM,
             10)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  if (OutputLength < TransformPrefixLength ||
      InputLength > OutputLength - TransformPrefixLength) {
    WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
    return;
  }
  Status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID *)&Input,
                                         &RetrievedInputLength);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  Status = WdfRequestRetrieveOutputBuffer(
      Request, InputLength + TransformPrefixLength, (PVOID *)&Output,
      &RetrievedOutputLength);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  if (!Check(Input != NULL && Output == Input &&
                 RetrievedInputLength == InputLength &&
                 RetrievedOutputLength == OutputLength,
             11)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  if (TransferMode == 'M' &&
      !CheckBufferedIoctlMdl(Request, Input, InputLength)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }

  // METHOD_BUFFERED aliases the input and output allocation; move backwards.
  for (size_t Index = InputLength; Index != 0; --Index)
    Output[Index - 1 + TransformPrefixLength] =
        Input[Index - 1] ^ TransformMask;
  Output[0] = 'K';
  Output[1] = 'M';
  Output[2] = 'D';
  Output[3] = TransferMode;
  DbgPrint("KMDF control: transformed %llu bytes in mode %c\n",
           (unsigned long long)InputLength, TransferMode);
  if (TransferMode == 'C') {
    CompleteObservedTransform(Request, Output,
                              InputLength + TransformPrefixLength);
    return;
  }
  if (TransferMode == 'M') {
    CompleteWithMetadata(Request, InputLength + TransformPrefixLength);
    return;
  }
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                    InputLength + TransformPrefixLength);
}

static void CompleteWorker(PDEVICE_OBJECT Device, PVOID Context) {
  DEFERRED_IOCTL_CONTEXT *Work = Context;
  WDFREQUEST Request = Work->Request;
  size_t OutputLength = Work->OutputLength;
  size_t InputLength = Work->InputLength;
  BOOLEAN Valid =
      Check(Work == &Deferred &&
                Device == WdfDeviceWdmGetDeviceObject(CreatedDevice) &&
                KeGetCurrentIrql() == PASSIVE_LEVEL,
            20);

  // A dequeued work item can free its own allocation. The request remains
  // driver-owned until completion, which also permits the next queue request.
  IoFreeWorkItem(Work->Item);
  Work->Item = NULL;
  Work->Request = NULL;
  if (!Valid) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  DbgPrint("KMDF control: deferred worker\n");
  TransformRequest(Request, OutputLength, InputLength);
}

static void CancelCleanup(WDFOBJECT Object) {
  CANCEL_REQUEST_CONTEXT *Context = CancelContext(Object);
  if (Check(Context->Cookie == 0xca1ce133 && Context->Phase == 1 &&
                KeGetCurrentIrql() == PASSIVE_LEVEL,
            80)) {
    Context->Phase = 2;
    DbgPrint("KMDF control: %c request cleanup\n", Context->Mode);
  }
}

static void CancelDestroy(WDFOBJECT Object) {
  CANCEL_REQUEST_CONTEXT *Context = CancelContext(Object);
  volatile ULONG SavedCookie = Context->Cookie;
  LARGE_INTEGER Delay;
  if (!Check(Context->Cookie == 0xca1ce133 && !Context->CallbackActive &&
                 Context->Phase == (Context->DeferredDestroy ? 3u : 2u) &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL,
             81))
    return;
  Context->Phase = 4;
  DbgPrint("KMDF control: %c request destroy\n", Context->Mode);
  if (!Context->DeferredDestroy)
    return;
  Delay.QuadPart = -7;
  if (Check(KeDelayExecutionThread(KernelMode, FALSE, &Delay) ==
                    STATUS_SUCCESS &&
                SavedCookie == 0xca1ce133 && Context->Cookie == SavedCookie &&
                Context->Phase == 4,
            82))
    DbgPrint("KMDF control: %c destroy resumed\n", Context->Mode);
}

static void CancellationWorker(PDEVICE_OBJECT Device, PVOID Context) {
  DEFERRED_IOCTL_CONTEXT *Work = Context;
  WDFREQUEST Request = Work->Request;
  size_t OutputLength = Work->OutputLength;
  size_t InputLength = Work->InputLength;
  LARGE_INTEGER Delay;
  BOOLEAN Valid =
      Check(Work == &Deferred &&
                Device == WdfDeviceWdmGetDeviceObject(CreatedDevice) &&
                KeGetCurrentIrql() == PASSIVE_LEVEL,
            83);
  IoFreeWorkItem(Work->Item);
  Work->Item = NULL;
  Work->Request = NULL;
  if (!Valid)
    return;
  if (TransferMode == 'U') {
    Delay.QuadPart = -20;
    if (!Check(KeDelayExecutionThread(KernelMode, FALSE, &Delay) ==
                   STATUS_SUCCESS,
               84))
      return;
    DbgPrint("KMDF control: U worker cancelled=%u\n",
             (unsigned)WdfRequestIsCanceled(Request));
    TransformRequest(Request, OutputLength, InputLength);
    return;
  }

  CANCEL_REQUEST_CONTEXT *Cancel = CancelContext(Request);
  // The running cancel callback explicitly delegated completion and waits
  // for this worker. STATUS_CANCELLED alone is not permission to complete:
  // this handshake establishes that cancellation already owns the request.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestunmarkcancelable
  if (!Check(Cancel->CallbackActive && Cancel->Phase == 1 &&
                 WdfRequestUnmarkCancelable(Request) == STATUS_CANCELLED,
             85))
    return;
  DbgPrint("KMDF control: H worker owns completion\n");
  WdfRequestComplete(Request, STATUS_CANCELLED);
  if (!Check(CancelContext(Request) == Cancel && Cancel->Cookie == 0xca1ce133 &&
                 Cancel->Phase == 2 && Cancel->CallbackActive,
             86))
    return;
  KeSetEvent(&Cancel->Completed, IO_NO_INCREMENT, FALSE);
}

static BOOLEAN QueueCancellationWorker(WDFREQUEST Request, size_t OutputLength,
                                       size_t InputLength) {
  if (!Check(Deferred.Item == NULL && Deferred.Request == NULL, 87))
    return FALSE;
  Deferred.Item =
      IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(CreatedDevice));
  if (!Check(Deferred.Item != NULL, 88))
    return FALSE;
  Deferred.Request = Request;
  Deferred.OutputLength = OutputLength;
  Deferred.InputLength = InputLength;
  IoQueueWorkItem(Deferred.Item, CancellationWorker, DelayedWorkQueue,
                  &Deferred);
  return TRUE;
}

static void CancelRequest(WDFREQUEST Request) {
  CANCEL_REQUEST_CONTEXT *Context = CancelContext(Request);
  if (!Check(Context->Cookie == 0xca1ce133 && Context->Phase == 0 &&
                 !Context->CallbackActive &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL,
             89))
    return;
  Context->Phase = 1;
  Context->CallbackActive = TRUE;
  Context->DeferredDestroy = TRUE;
  DbgPrint("KMDF control: %c cancel callback\n", Context->Mode);
  if (Context->Mode == 'H') {
    if (!QueueCancellationWorker(Request, 0, 0))
      return;
    if (!Check(KeWaitForSingleObject(&Context->Completed, Executive, KernelMode,
                                     FALSE, NULL) == STATUS_SUCCESS,
               90))
      return;
  } else {
    WdfRequestComplete(Request, STATUS_CANCELLED);
  }
  // No external WdfObjectReference is held here. The framework's cancel
  // callback hold must preserve typed context until this callback returns.
  if (Check(CancelContext(Request) == Context &&
                Context->Cookie == 0xca1ce133 && Context->Phase == 2 &&
                Context->CallbackActive,
            91)) {
    Context->Phase = 3;
    Context->CallbackActive = FALSE;
    DbgPrint("KMDF control: %c cancel callback retained context\n",
             Context->Mode);
  }
}

static void CancelableIoctl(WDFREQUEST Request, size_t OutputLength,
                            size_t InputLength) {
  CANCEL_REQUEST_CONTEXT *Context = NULL;
  WDF_OBJECT_ATTRIBUTES Attributes;
  NTSTATUS Status;
  if (TransferMode == 'X' || TransferMode == 'H' || TransferMode == 'L') {
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes,
                                            CANCEL_REQUEST_CONTEXT);
    Attributes.EvtCleanupCallback = CancelCleanup;
    Attributes.EvtDestroyCallback = CancelDestroy;
    Status = WdfObjectAllocateContext(Request, &Attributes, (PVOID *)&Context);
    if (!NT_SUCCESS(Status)) {
      WdfRequestComplete(Request, Status);
      return;
    }
    Context->Cookie = 0xca1ce133;
    Context->Mode = TransferMode;
    if (TransferMode == 'H')
      KeInitializeEvent(&Context->Completed, NotificationEvent, FALSE);
  }
  if (TransferMode == 'L') {
    DbgPrint("KMDF control: L before mark\n");
    WdfRequestMarkCancelable(Request, CancelRequest);
    // With pre-dispatch cancellation, even the context can already have been
    // destroyed here. Do not inspect the request or its saved context pointer.
    DbgPrint("KMDF control: L after mark\n");
    return;
  }
  Status = WdfRequestMarkCancelableEx(Request, CancelRequest);
  if (Status == STATUS_CANCELLED) {
    if (!Check(WdfRequestIsCanceled(Request), 92))
      return;
    if (Context)
      Context->Phase = 1;
    DbgPrint("KMDF control: %c already cancelled\n", TransferMode);
    WdfRequestComplete(Request, STATUS_CANCELLED);
    return;
  }
  if (!Check(Status == STATUS_SUCCESS, 93))
    return;
  DbgPrint("KMDF control: %c marked cancelable\n", TransferMode);
  if (TransferMode == 'N') {
    // Deliberate verifier violation: no cancel callback has begun and the
    // driver has not successfully unmarked the request before completion.
    WdfRequestComplete(Request, STATUS_SUCCESS);
    DbgPrint("KMDF control: forbidden marked completion returned\n");
    return;
  }
  if (TransferMode == 'U') {
    if (!Check(WdfRequestUnmarkCancelable(Request) == STATUS_SUCCESS, 94))
      return;
    DbgPrint("KMDF control: U unmarked cancelable\n");
    QueueCancellationWorker(Request, OutputLength, InputLength);
  }
}

static void IoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                            size_t OutputLength, size_t InputLength,
                            ULONG IoControlCode) {
  if (TransferMode == 'I' && !Check(CallerRequestCount == 1, 111)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  if (!CheckQueue(Queue, 30)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  // IoControlCode is the fifth Windows x64 argument, passed on the stack.
  if (IoControlCode != IOCTL_NEVERD_KMDF_TRANSFORM) {
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
    return;
  }
  if (TransferMode == 'X' || TransferMode == 'H' || TransferMode == 'U' ||
      TransferMode == 'N' || TransferMode == 'L') {
    CancelableIoctl(Request, OutputLength, InputLength);
    return;
  }
  if (TransferMode == 'B' && WdfRequestIsCanceled(Request)) {
    DbgPrint("KMDF control: B observed cancellation\n");
    WdfRequestComplete(Request, STATUS_CANCELLED);
    return;
  }
  if (TransferMode != 'W') {
    TransformRequest(Request, OutputLength, InputLength);
    return;
  }
  if (!Check(Deferred.Item == NULL && Deferred.Request == NULL, 31)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  Deferred.Item =
      IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(CreatedDevice));
  if (Deferred.Item == NULL) {
    WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
    return;
  }
  Deferred.Request = Request;
  Deferred.OutputLength = OutputLength;
  Deferred.InputLength = InputLength;
  IoQueueWorkItem(Deferred.Item, CompleteWorker, DelayedWorkQueue, &Deferred);
  DbgPrint("KMDF control: queued deferred request\n");
}

static void IoRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length) {
  if (TransferMode == 'I' && !Check(CallerRequestCount == 2, 112)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  WDF_REQUEST_PARAMETERS Parameters;
  PUCHAR Buffer = NULL;
  size_t RetrievedLength = 0;
  NTSTATUS Status;

  WDF_REQUEST_PARAMETERS_INIT(&Parameters);
  WdfRequestGetParameters(Request, &Parameters);
  if (!CheckQueue(Queue, 40) ||
      !Check(Parameters.Type == WdfRequestTypeRead &&
                 Parameters.Parameters.Read.Length == Length,
             41)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  Status = WdfRequestRetrieveOutputBuffer(Request, Length, (PVOID *)&Buffer,
                                          &RetrievedLength);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  if (!Check(Buffer != NULL && RetrievedLength == Length, 42)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  if ((TransferMode == 'M' || TransferMode == 'D') &&
      !CheckTransferMdl(Request, Buffer, Length, FALSE)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  for (size_t Index = 0; Index != Length; ++Index)
    Buffer[Index] = (UCHAR)(ReadPatternBase + (Index & PatternIndexMask));
  DbgPrint("KMDF control: read %llu bytes\n", (unsigned long long)Length);
  if (TransferMode == 'M' || TransferMode == 'D') {
    CompleteWithMetadata(Request, Length);
    return;
  }
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
}

static void IoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length) {
  if (TransferMode == 'I' && !Check(CallerRequestCount == 3, 113)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  WDF_REQUEST_PARAMETERS Parameters;
  PUCHAR Buffer = NULL;
  size_t RetrievedLength = 0;
  NTSTATUS Status;

  WDF_REQUEST_PARAMETERS_INIT(&Parameters);
  WdfRequestGetParameters(Request, &Parameters);
  if (!CheckQueue(Queue, 50) ||
      !Check(Parameters.Type == WdfRequestTypeWrite &&
                 Parameters.Parameters.Write.Length == Length,
             51)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  Status = WdfRequestRetrieveInputBuffer(Request, Length, (PVOID *)&Buffer,
                                         &RetrievedLength);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  if (!Check(Buffer != NULL && RetrievedLength == Length, 52)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  if ((TransferMode == 'M' || TransferMode == 'D') &&
      !CheckTransferMdl(Request, Buffer, Length, TRUE)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  for (size_t Index = 0; Index != Length; ++Index) {
    if (Buffer[Index] !=
        (UCHAR)(WritePatternBase + (Index & PatternIndexMask))) {
      WdfRequestComplete(Request, STATUS_DATA_ERROR);
      return;
    }
  }
  DbgPrint("KMDF control: write %llu bytes\n", (unsigned long long)Length);
  if (TransferMode == 'M' || TransferMode == 'D') {
    CompleteWithMetadata(Request, Length);
    return;
  }
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
}

static void DriverUnload(WDFDRIVER Driver) {
  (void)Driver;
  Check(Deferred.Item == NULL && Deferred.Request == NULL &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        60);
  WdfObjectDelete(CreatedDevice);
  CreatedDevice = NULL;
  DefaultQueue = NULL;
  DbgPrint("KMDF control: driver unload\n");
}

static void IoInCallerContext(WDFDEVICE Device, WDFREQUEST Request) {
  WDF_REQUEST_PARAMETERS Parameters;
  WDF_REQUEST_PARAMETERS_INIT(&Parameters);
  WdfRequestGetParameters(Request, &Parameters);
  if (!Check(Device == CreatedDevice && WdfRequestGetIoQueue(Request) == NULL &&
                 KeGetCurrentIrql() == PASSIVE_LEVEL &&
                 (ULONG)(ULONG_PTR)PsGetCurrentProcessId() ==
                     IoGetRequestorProcessId(WdfRequestWdmGetIrp(Request)) &&
                 (Parameters.Type == WdfRequestTypeRead ||
                  Parameters.Type == WdfRequestTypeWrite ||
                  Parameters.Type == WdfRequestTypeDeviceControl),
             110)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  ++CallerRequestCount;
  if (TransferMode == 'J')
    return;
  if (TransferMode == 'K') {
    WdfRequestComplete(Request, STATUS_SUCCESS);
    return;
  }
  NTSTATUS Status = WdfDeviceEnqueueRequest(Device, Request);
  if (!NT_SUCCESS(Status))
    WdfRequestComplete(Request, Status);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG DriverConfig;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_IO_QUEUE_CONFIG QueueConfig;
  WDFDRIVER Driver = NULL;
  PWDFDEVICE_INIT DeviceInit = NULL;
  PDEVICE_OBJECT DeviceObject;
  UNICODE_STRING Security;
  UNICODE_STRING Name;
  UNICODE_STRING Link;
  NTSTATUS Status;
  WCHAR Marker =
      RegistryPath->Length >= sizeof(WCHAR)
          ? RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1]
          : L'B';
  TransferMode = Marker == L'D'   ? 'D'
                 : Marker == L'W' ? 'W'
                 : Marker == L'C' ? 'C'
                 : Marker == L'X' ? 'X'
                 : Marker == L'H' ? 'H'
                 : Marker == L'U' ? 'U'
                 : Marker == L'N' ? 'N'
                 : Marker == L'L' ? 'L'
                 : Marker == L'M' ? 'M'
                 : Marker == L'I' ? 'I'
                 : Marker == L'J' ? 'J'
                 : Marker == L'K' ? 'K'
                                  : 'B';

  WDF_DRIVER_CONFIG_INIT(&DriverConfig, WDF_NO_EVENT_CALLBACK);
  DriverConfig.DriverInitFlags = WdfDriverInitNonPnpDriver;
  DriverConfig.EvtDriverUnload = DriverUnload;
  DriverConfig.DriverPoolTag = 0x4e444b43;
  Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &DriverConfig, &Driver);
  if (!NT_SUCCESS(Status))
    return Status;

  // This fixture intentionally grants universal access. It does not rely on
  // host tokens or imply a security policy for any restricted SDDL descriptor.
  RtlInitUnicodeString(&Security, L"D:P(A;;GA;;;WD)");
  DeviceInit = WdfControlDeviceInitAllocate(Driver, &Security);
  if (DeviceInit == NULL)
    return STATUS_INSUFFICIENT_RESOURCES;
  WdfDeviceInitSetIoType(DeviceInit, TransferMode == 'D' ? WdfDeviceIoDirect
                                                         : WdfDeviceIoBuffered);
  if (TransferMode == 'I' || TransferMode == 'J' || TransferMode == 'K')
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, IoInCallerContext);
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDKmdfControl");
  Status = WdfDeviceInitAssignName(DeviceInit, &Name);
  if (!NT_SUCCESS(Status))
    goto Failure;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Status = WdfDeviceCreate(&DeviceInit, &Attributes, &CreatedDevice);
  if (!NT_SUCCESS(Status))
    goto Failure;
  if (!Check(DeviceInit == NULL && CreatedDevice != NULL, 1)) {
    Status = STATUS_UNSUCCESSFUL;
    goto Failure;
  }
  RtlInitUnicodeString(&Link, L"\\DosDevices\\NeverDKmdfControl");
  Status = WdfDeviceCreateSymbolicLink(CreatedDevice, &Link);
  if (!NT_SUCCESS(Status))
    goto Failure;

  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig,
                                         WdfIoQueueDispatchSequential);
  QueueConfig.EvtIoDeviceControl = IoDeviceControl;
  QueueConfig.EvtIoRead = IoRead;
  QueueConfig.EvtIoWrite = IoWrite;
  if (Marker == L'Q')
    QueueConfig.Size = 0;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Status =
      WdfIoQueueCreate(CreatedDevice, &QueueConfig, &Attributes, &DefaultQueue);
  if (!NT_SUCCESS(Status))
    goto Failure;
  if (Marker == L'Q' ||
      !Check(DefaultQueue != NULL &&
                 WdfDeviceGetDefaultQueue(CreatedDevice) == DefaultQueue,
             2)) {
    Status = STATUS_UNSUCCESSFUL;
    goto Failure;
  }

  DeviceObject = WdfDeviceWdmGetDeviceObject(CreatedDevice);
  if (!Check(DeviceObject != NULL &&
                 (DeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO)) ==
                     (TransferMode == 'D' ? DO_DIRECT_IO : DO_BUFFERED_IO) &&
                 (DeviceObject->Flags & DO_DEVICE_INITIALIZING) != 0,
             3)) {
    Status = STATUS_UNSUCCESSFUL;
    goto Failure;
  }
  WdfControlFinishInitializing(CreatedDevice);
  if (!Check((DeviceObject->Flags & DO_DEVICE_INITIALIZING) == 0, 4)) {
    Status = STATUS_UNSUCCESSFUL;
    goto Failure;
  }
  DbgPrint("KMDF control: ready in mode %c\n", TransferMode);
  return STATUS_SUCCESS;

Failure:
  if (DeviceInit != NULL)
    WdfDeviceInitFree(DeviceInit);
  if (CreatedDevice != NULL) {
    WdfObjectDelete(CreatedDevice);
    CreatedDevice = NULL;
    DefaultQueue = NULL;
  }
  return Status;
}
