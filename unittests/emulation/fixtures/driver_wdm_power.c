//===- driver_wdm_power.c - Genuine WDK power IRP fixture ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original resource-free power-policy driver, built with genuine WDK headers
/// and libraries. S exercises nested policy, E early S0 completion, W a waiting
/// power callback, I independent worker requests, N a null callback, Q rejected
/// device queries, and T two independent devices with different explicit seeds.
///
//===----------------------------------------------------------------------===//

#include <ntddk.h>

#define ABI_OFFSET(Type, Member, Value)                                        \
  _Static_assert(__builtin_offsetof(Type, Member) == Value, #Type "." #Member)
_Static_assert(sizeof(void *) == 8 && sizeof(IRP) == 0xd0 &&
                   sizeof(IO_STACK_LOCATION) == 0x48,
               "x64 WDM packet ABI");
_Static_assert(sizeof(POWER_STATE) == 4 &&
                   sizeof(SYSTEM_POWER_STATE_CONTEXT) == 4 &&
                   sizeof(IO_STATUS_BLOCK) == 16,
               "power value and callback snapshot ABI");
ABI_OFFSET(IO_STACK_LOCATION, Parameters.Power.SystemContext, 8);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.Power.Type, 16);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.Power.State, 24);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.Power.ShutdownType, 32);
ABI_OFFSET(IO_STATUS_BLOCK, Information, 8);
_Static_assert(IRP_MJ_POWER == 0x16 && IRP_MN_SET_POWER == 2 &&
                   IRP_MN_QUERY_POWER == 3 && PowerDeviceD0 == 1 &&
                   PowerDeviceD3 == 4 && PowerSystemSleeping3 == 4 &&
                   DO_POWER_PAGABLE == 0x2000 && DO_POWER_INRUSH == 0x4000,
               "public WDM power codes");
typedef NTSTATUS (*POWER_REQUEST_ABI)(PDEVICE_OBJECT, UCHAR, POWER_STATE,
                                      PREQUEST_POWER_COMPLETE, PVOID, PIRP *);
typedef VOID (*POWER_CALLBACK_ABI)(PDEVICE_OBJECT, UCHAR, POWER_STATE, PVOID,
                                   PIO_STATUS_BLOCK);
_Static_assert(__builtin_types_compatible_p(__typeof__(&PoRequestPowerIrp),
                                            POWER_REQUEST_ABI) &&
                   __builtin_types_compatible_p(PREQUEST_POWER_COMPLETE,
                                                POWER_CALLBACK_ABI),
               "six request arguments and five callback arguments");

#define CONTEXT_TAG 0x7257504eUL

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  KEVENT PnpEvent;
  KEVENT ChildEvent;
  PIO_WORKITEM WorkItem;
  PIRP WorkerIrp;
  PIRP SystemIrp;
  DEVICE_POWER_STATE Reported;
  ULONG Unit;
  ULONG DeviceCompletions;
  ULONG PowerCallbacks;
  BOOLEAN QueryRejected;
  BOOLEAN EarlyCompleted;
} POWER_EXTENSION;

typedef struct {
  POWER_EXTENSION *Extension;
  PDEVICE_OBJECT RequestedDevice;
  PIRP Parent;
  POWER_STATE State;
  UCHAR Minor;
  BOOLEAN Worker;
  BOOLEAN Early;
  ULONG CompletionBaseline;
  ULONG Cookie;
} POWER_CONTEXT;

static UCHAR Mode;
static ULONG AddCalls;
static ULONG LiveDevices;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM power: failure %lu\n", Code);
  return Condition;
}

static NTSTATUS Finish(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static void Notify(POWER_EXTENSION *Extension, DEVICE_POWER_STATE State) {
  POWER_STATE Value;
  Value.DeviceState = State;
  POWER_STATE Previous =
      PoSetPowerState(Extension->Self, DevicePowerState, Value);
  Check(Previous.DeviceState == Extension->Reported, 10);
  DbgPrint("WDM power: notify unit=%lu old=%u new=%u\n", Extension->Unit,
           (unsigned)Previous.DeviceState, (unsigned)State);
  Extension->Reported = State;
}

static NTSTATUS PnpCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Irp->IoStatus.Information == 0,
        11);
  KeSetEvent(&Extension->PnpEvent, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  const UCHAR Minor = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
  NTSTATUS Status;
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Lower, Irp);
    IoDetachDevice(Lower);
    --LiveDevices;
    IoDeleteDevice(Device);
    DbgPrint("WDM power: removed\n");
    return Status;
  }
  if (Minor != IRP_MN_START_DEVICE && Minor != IRP_MN_QUERY_REMOVE_DEVICE &&
      Minor != IRP_MN_CANCEL_REMOVE_DEVICE)
    return Finish(Irp, STATUS_NOT_SUPPORTED, 0);
  KeClearEvent(&Extension->PnpEvent);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, PnpCompletion, Extension, TRUE, TRUE, TRUE);
  Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(KeWaitForSingleObject(&Extension->PnpEvent, Executive, KernelMode,
                                FALSE, NULL) == STATUS_SUCCESS,
          12);
  Status = Irp->IoStatus.Status;
  if (Minor == IRP_MN_START_DEVICE && NT_SUCCESS(Status))
    Notify(Extension, PowerDeviceD0);
  return Finish(Irp, Status, 0);
}

static NTSTATUS DeviceCompletion(PDEVICE_OBJECT Device, PIRP Irp,
                                 PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  Check(Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Stack->Parameters.Power.Type == DevicePowerState &&
            Irp->IoStatus.Information == 0,
        20);
  ++Extension->DeviceCompletions;
  DbgPrint("WDM power: device-completion unit=%lu minor=%u state=%u\n",
           Extension->Unit, (unsigned)Stack->MinorFunction,
           (unsigned)Stack->Parameters.Power.State.DeviceState);
  if (Stack->MinorFunction == IRP_MN_SET_POWER &&
      Stack->Parameters.Power.State.DeviceState == PowerDeviceD0 &&
      NT_SUCCESS(Irp->IoStatus.Status))
    Notify(Extension, PowerDeviceD0);
  if (Irp->PendingReturned)
    IoMarkIrpPending(Irp);
  return STATUS_SUCCESS;
}

static REQUEST_POWER_COMPLETE PowerComplete;
static VOID PowerComplete(PDEVICE_OBJECT Device, UCHAR Minor, POWER_STATE State,
                          PVOID Context, PIO_STATUS_BLOCK IoStatus) {
  POWER_CONTEXT *Power = Context;
  POWER_EXTENSION *Extension = Power->Extension;
  const NTSTATUS FinalStatus = IoStatus->Status;
  volatile ULONG Canary = Power->Cookie;
  Check(Device == Power->RequestedDevice && Minor == Power->Minor &&
            State.DeviceState == Power->State.DeviceState &&
            KeGetCurrentIrql() == PASSIVE_LEVEL && IoStatus->Information == 0 &&
            Canary == 0xabc00000 + Extension->Unit &&
            Extension->DeviceCompletions == Power->CompletionBaseline + 1,
        21);
  ++Extension->PowerCallbacks;
  DbgPrint("WDM power: callback unit=%lu minor=%u state=%u status=0x%08lx\n",
           Extension->Unit, (unsigned)Minor, (unsigned)State.DeviceState,
           (ULONG)FinalStatus);
  if (Mode == 'W') {
    LARGE_INTEGER Delay;
    Delay.QuadPart = -7;
    Check(KeDelayExecutionThread(KernelMode, FALSE, &Delay) == STATUS_SUCCESS &&
              Canary == Power->Cookie && IoStatus->Status == FinalStatus &&
              IoStatus->Information == 0 && Device == Power->RequestedDevice,
          22);
    DbgPrint("WDM power: callback snapshot survived wait\n");
  }
  if (Power->Early) {
    Check(Extension->EarlyCompleted, 23);
    DbgPrint("WDM power: D0 child observed completed S0\n");
  }
  if (Power->Parent) {
    PIRP Parent = Power->Parent;
    Extension->SystemIrp = NULL;
    Finish(Parent, FinalStatus, 0);
    DbgPrint("WDM power: parent completed after child\n");
  }
  if (Power->Worker)
    KeSetEvent(&Extension->ChildEvent, IO_NO_INCREMENT, FALSE);
  ExFreePoolWithTag(Power, CONTEXT_TAG);
}

static POWER_CONTEXT *NewContext(POWER_EXTENSION *Extension, UCHAR Minor,
                                 DEVICE_POWER_STATE State) {
  POWER_CONTEXT *Context =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(POWER_CONTEXT), CONTEXT_TAG);
  if (!Context)
    return NULL;
  Context->Extension = Extension;
  Context->RequestedDevice = Extension->PDO;
  Context->Parent = NULL;
  Context->Minor = Minor;
  Context->State.DeviceState = State;
  Context->Worker = FALSE;
  Context->Early = FALSE;
  Context->CompletionBaseline = Extension->DeviceCompletions;
  Context->Cookie = 0xabc00000 + Extension->Unit;
  return Context;
}

static NTSTATUS SystemCompletion(PDEVICE_OBJECT Device, PIRP Irp,
                                 PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  const UCHAR Minor = Stack->MinorFunction;
  const BOOLEAN Waking =
      Stack->Parameters.Power.State.SystemState == PowerSystemWorking;
  Check(Device == Extension->Self && Extension->SystemIrp == Irp, 30);
  if (!NT_SUCCESS(Irp->IoStatus.Status)) {
    Extension->SystemIrp = NULL;
    if (Irp->PendingReturned)
      IoMarkIrpPending(Irp);
    return STATUS_SUCCESS;
  }
  POWER_CONTEXT *Power =
      NewContext(Extension, Minor, Waking ? PowerDeviceD0 : PowerDeviceD3);
  if (!Power)
    return STATUS_MORE_PROCESSING_REQUIRED;
  const BOOLEAN Early = Mode == 'E' && Waking && Minor == IRP_MN_SET_POWER;
  Power->Parent = Early ? NULL : Irp;
  Power->Early = Early;
  Extension->EarlyCompleted = FALSE;
  DbgPrint("WDM power: request child before API\n");
  const NTSTATUS Status = PoRequestPowerIrp(Extension->PDO, Minor, Power->State,
                                            PowerComplete, Power, NULL);
  // A synchronous callback may already have freed Power and completed Irp.
  Check(Status == STATUS_PENDING, 31);
  DbgPrint("WDM power: request child returned pending\n");
  if (Early) {
    Extension->SystemIrp = NULL;
    Finish(Irp, STATUS_SUCCESS, 0);
    Extension->EarlyCompleted = TRUE;
    DbgPrint("WDM power: S0 completed before child\n");
  }
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Device, PIRP Irp) {
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  const UCHAR Minor = Stack->MinorFunction;
  const POWER_STATE_TYPE Type = Stack->Parameters.Power.Type;
  const ULONG State = Stack->Parameters.Power.State.DeviceState;
  const POWER_ACTION Action = Stack->Parameters.Power.ShutdownType;
  Check(KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Stack->MajorFunction == IRP_MJ_POWER &&
            Stack->DeviceObject == Device && Stack->FileObject == NULL &&
            Irp->Tail.Overlay.OriginalFileObject == NULL &&
            Irp->RequestorMode == KernelMode && Irp->MdlAddress == NULL &&
            Irp->AssociatedIrp.SystemBuffer == NULL &&
            Irp->UserBuffer == NULL &&
            (Minor == IRP_MN_QUERY_POWER || Minor == IRP_MN_SET_POWER) &&
            (State == 1 || State == 4),
        32);
  if (Action == PowerActionSleep) {
    Check(Stack->Parameters.Power.SystemPowerStateContext.TargetSystemState ==
                  State &&
              Stack->Parameters.Power.SystemPowerStateContext
                      .EffectiveSystemState == State,
          33);
  } else {
    Check(Type == DevicePowerState && Action == PowerActionNone, 34);
  }
  DbgPrint("WDM power: dispatch unit=%lu type=%u minor=%u state=%lu\n",
           Extension->Unit, (unsigned)Type, (unsigned)Minor, State);
  PoStartNextPowerIrp(Irp);
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Type == DevicePowerState) {
    Check(Extension->SystemIrp != Irp, 35);
    if (Mode == 'Q' && Minor == IRP_MN_QUERY_POWER && State == PowerDeviceD3 &&
        !Extension->QueryRejected) {
      Extension->QueryRejected = TRUE;
      DbgPrint("WDM power: rejected device query\n");
      return Finish(Irp, STATUS_UNSUCCESSFUL, 0);
    }
    if (Minor == IRP_MN_SET_POWER && State == PowerDeviceD3)
      Notify(Extension, PowerDeviceD3);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, DeviceCompletion, Extension, TRUE, TRUE, TRUE);
    return PoCallDriver(Extension->Lower, Irp);
  }
  Check(Type == SystemPowerState && Extension->SystemIrp == NULL, 36);
  Extension->SystemIrp = Irp;
  IoMarkIrpPending(Irp);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, SystemCompletion, Extension, TRUE, TRUE, TRUE);
  PoCallDriver(Extension->Lower, Irp);
  return STATUS_PENDING;
}

static void PowerWorker(PDEVICE_OBJECT Device, PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  POWER_CONTEXT *Power = NewContext(Extension, IRP_MN_SET_POWER, PowerDeviceD3);
  PIRP Irp = Extension->WorkerIrp;
  PIO_WORKITEM Work = Extension->WorkItem;
  Check(Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL, 40);
  if (!Power)
    return;
  Power->RequestedDevice = Device;
  Power->Worker = TRUE;
  const NTSTATUS Status = PoRequestPowerIrp(
      Device, IRP_MN_SET_POWER, Power->State,
      Mode == 'N' ? NULL : PowerComplete, Mode == 'N' ? NULL : Power, NULL);
  Check(Status == STATUS_PENDING, 41);
  DbgPrint("WDM power: worker request returned pending\n");
  if (Mode == 'N') {
    ExFreePoolWithTag(Power, CONTEXT_TAG);
    Check(Extension->PowerCallbacks == 0 && Extension->DeviceCompletions == 1,
          42);
    DbgPrint("WDM power: null callback child completed\n");
  } else {
    Check(KeWaitForSingleObject(&Extension->ChildEvent, Executive, KernelMode,
                                FALSE, NULL) == STATUS_SUCCESS,
          43);
  }
  PUCHAR Buffer = Irp->AssociatedIrp.SystemBuffer;
  Buffer[0] = 'P';
  Buffer[1] = 'W';
  Buffer[2] = 'R';
  Buffer[3] = '!';
  Extension->WorkerIrp = NULL;
  Extension->WorkItem = NULL;
  Finish(Irp, STATUS_SUCCESS, 4);
  IoFreeWorkItem(Work);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  Check(Stack->FileObject != NULL &&
            Stack->FileObject->DeviceObject == Extension->PDO,
        44);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Finish(Irp, STATUS_SUCCESS, 0);
  if ((Mode != 'I' && Mode != 'N') ||
      Stack->Parameters.DeviceIoControl.IoControlCode != 0x222000 ||
      Stack->Parameters.DeviceIoControl.OutputBufferLength < 4)
    return Finish(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
  Extension->WorkItem = IoAllocateWorkItem(Device);
  if (!Extension->WorkItem)
    return Finish(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
  Extension->WorkerIrp = Irp;
  KeClearEvent(&Extension->ChildEvent);
  IoMarkIrpPending(Irp);
  IoQueueWorkItem(Extension->WorkItem, PowerWorker, DelayedWorkQueue,
                  Extension);
  return STATUS_PENDING;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  Check((PDO->Flags & (DO_POWER_PAGABLE | DO_POWER_INRUSH)) == DO_POWER_PAGABLE,
        50);
  NTSTATUS Status =
      IoCreateDevice(Driver, sizeof(POWER_EXTENSION), NULL, FILE_DEVICE_UNKNOWN,
                     FILE_DEVICE_SECURE_OPEN, FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Unit = ++AddCalls;
  Extension->Reported =
      Mode == 'T' && Extension->Unit == 2 ? PowerDeviceD0 : PowerDeviceD3;
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++LiveDevices;
  KeInitializeEvent(&Extension->PnpEvent, NotificationEvent, FALSE);
  KeInitializeEvent(&Extension->ChildEvent, NotificationEvent, FALSE);
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  DbgPrint("WDM power: added unit=%lu mode=%c\n", Extension->Unit, Mode);
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(LiveDevices == 0 && Driver->DeviceObject == NULL, 51);
  DbgPrint("WDM power: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  Mode = 'S';
  if (RegistryPath->Length >= sizeof(WCHAR)) {
    const WCHAR Last =
        RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1];
    if (Last == 'E' || Last == 'W' || Last == 'I' || Last == 'N' ||
        Last == 'Q' || Last == 'T')
      Mode = (UCHAR)Last;
  }
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  DbgPrint("WDM power: entry\n");
  return STATUS_SUCCESS;
}
