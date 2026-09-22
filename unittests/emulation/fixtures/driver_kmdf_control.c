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

#include <ntddk.h>
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
ABI_SLOT(WdfDeviceInitAssignName, 67);
ABI_SLOT(WdfDeviceCreate, 75);
ABI_SLOT(WdfDeviceCreateSymbolicLink, 80);
ABI_SLOT(WdfDeviceGetDefaultQueue, 92);
ABI_SLOT(WdfDriverCreate, 116);
ABI_SLOT(WdfIoQueueCreate, 152);
ABI_SLOT(WdfObjectAllocateContext, 203);
ABI_SLOT(WdfObjectReferenceActual, 205);
ABI_SLOT(WdfObjectDereferenceActual, 206);
ABI_SLOT(WdfObjectCreate, 207);
ABI_SLOT(WdfObjectDelete, 208);
ABI_SLOT(WdfRequestComplete, 263);
ABI_SLOT(WdfRequestCompleteWithInformation, 265);
ABI_SLOT(WdfRequestGetParameters, 266);
ABI_SLOT(WdfRequestRetrieveInputBuffer, 269);
ABI_SLOT(WdfRequestRetrieveOutputBuffer, 270);

typedef struct {
  WDFREQUEST Request;
  PIO_WORKITEM Item;
  size_t OutputLength;
  size_t InputLength;
} DEFERRED_IOCTL_CONTEXT;

typedef struct {
  PUCHAR Buffer;
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

static WDFDEVICE CreatedDevice;
static WDFQUEUE DefaultQueue;
static UCHAR TransferMode;
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
      CheckSavedOutput(Context, 71)) {
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
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
  // Only the context is inspected after completion; no request accessor or
  // saved buffer dereference may extend the underlying IRP's lifetime.
  if (Check(CleanupContext(Request) == Context &&
                Context->Cookie == 0xc0de3233 && Context->Phase == 2,
            75)) {
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

static void IoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                            size_t OutputLength, size_t InputLength,
                            ULONG IoControlCode) {
  if (!CheckQueue(Queue, 30)) {
    WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
    return;
  }
  // IoControlCode is the fifth Windows x64 argument, passed on the stack.
  if (IoControlCode != IOCTL_NEVERD_KMDF_TRANSFORM) {
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
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
  for (size_t Index = 0; Index != Length; ++Index)
    Buffer[Index] = (UCHAR)(ReadPatternBase + (Index & PatternIndexMask));
  DbgPrint("KMDF control: read %llu bytes\n", (unsigned long long)Length);
  WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
}

static void IoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length) {
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
  for (size_t Index = 0; Index != Length; ++Index) {
    if (Buffer[Index] !=
        (UCHAR)(WritePatternBase + (Index & PatternIndexMask))) {
      WdfRequestComplete(Request, STATUS_DATA_ERROR);
      return;
    }
  }
  DbgPrint("KMDF control: write %llu bytes\n", (unsigned long long)Length);
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
