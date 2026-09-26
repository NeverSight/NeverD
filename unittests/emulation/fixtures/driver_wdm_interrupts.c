//===- driver_wdm_interrupts.c - Genuine WDK interrupt fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original driver using genuine WDK interrupt declarations and resource lists.
/// A declared external pulse invokes a real ISR; its DPC completes a pending
/// IRP.
//===----------------------------------------------------------------------===//
#include <intrin.h>
#include <ntddk.h>

#define OFFSET(Type, Field, Value)                                             \
  _Static_assert(__builtin_offsetof(Type, Field) == Value, #Type "." #Field)
_Static_assert(sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 20 &&
                   _Alignof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 4 &&
                   sizeof(IO_CONNECT_INTERRUPT_PARAMETERS) == 80 &&
                   sizeof(IO_DISCONNECT_INTERRUPT_PARAMETERS) == 16 &&
                   sizeof(BOOLEAN) == 1 && sizeof(KIRQL) == 1,
               "genuine x64 interrupt ABI");
OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Interrupt.Level, 4);
OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Interrupt.Vector, 8);
OFFSET(CM_PARTIAL_RESOURCE_DESCRIPTOR, u.Interrupt.Affinity, 12);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.PhysicalDeviceObject, 8);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.InterruptObject, 16);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.ServiceRoutine, 24);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.ServiceContext, 32);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.SpinLock, 40);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.SynchronizeIrql, 48);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.FloatingSave, 49);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.ShareVector, 50);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.Vector, 52);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.Irql, 56);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.InterruptMode, 60);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.ProcessorEnableMask, 64);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, FullySpecified.Group, 72);
OFFSET(IO_CONNECT_INTERRUPT_PARAMETERS, LineBased.SynchronizeIrql, 48);
OFFSET(IO_DISCONNECT_INTERRUPT_PARAMETERS, ConnectionContext, 8);
_Static_assert(CmResourceTypeInterrupt == 2 &&
                   CmResourceShareDeviceExclusive == 1 &&
                   CM_RESOURCE_INTERRUPT_LATCHED == 1 && Latched == 1 &&
                   CONNECT_FULLY_SPECIFIED == 1 && CONNECT_LINE_BASED == 2 &&
                   CONNECT_FULLY_SPECIFIED_GROUP == 4,
               "genuine resource and connection constants");

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  KEVENT Event;
  KDPC Dpc;
  PKINTERRUPT Interrupt;
  PIRP Pending;
  PIRP Transfer;
  volatile ULONG *Register;
  ULONG MapLength;
  ULONG Unit;
  ULONG Starts;
  ULONG ISRs;
  ULONG DPCs;
  ULONG SyncTrue;
  ULONG SyncFalse;
  ULONG Manual;
  ULONG LastIRQL;
  ULONG Vector;
  ULONG ConnectVersion;
  KIRQL IRQL;
  UCHAR SyncAction;
  BOOLEAN Started;
  BOOLEAN Surprise;
  BOOLEAN RemovePending;
} INTERRUPT_EXTENSION;

static UCHAR Mode;
static ULONG Units;
static ULONG Live;
static KSPIN_LOCK SharedInterruptLock;

static KIRQL ReadGuestCR8(void) {
  ULONG64 Value;
  __asm__ volatile("movq %%cr8, %0" : "=r"(Value));
  return (KIRQL)Value;
}

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM interrupts: failure %lu\n", Code);
  return Condition;
}

static void CheckIRQL(KIRQL Expected, ULONG Code) {
  Check(KeGetCurrentIrql() == Expected && ReadGuestCR8() == Expected, Code);
}

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static void Snapshot(INTERRUPT_EXTENSION *Extension, PULONG Values) {
  Values[0] = Extension->Unit;
  Values[1] = Extension->Starts;
  Values[2] = Extension->ISRs;
  Values[3] = Extension->DPCs;
  Values[4] = Extension->SyncTrue;
  Values[5] = Extension->SyncFalse;
  Values[6] = Extension->Manual;
  Values[7] = Extension->LastIRQL;
}

static BOOLEAN Critical(PVOID Context) {
  INTERRUPT_EXTENSION *Extension = Context;
  CheckIRQL(Extension->IRQL, 10);
  if (Mode == 'H')
    KeSynchronizeExecution(Extension->Interrupt, Critical, Extension);
  if (Extension->SyncAction == 1) {
    ++Extension->SyncFalse;
    return FALSE;
  }
  ++Extension->SyncTrue;
  if (Extension->SyncAction == 2) {
    Check(Extension->Pending == NULL, 11);
    Extension->Pending = Extension->Transfer;
  } else if (Extension->SyncAction == 3) {
    Extension->Transfer = Extension->Pending;
    Extension->Pending = NULL;
  }
  return TRUE;
}

static void ManualLock(INTERRUPT_EXTENSION *Extension, KIRQL Expected) {
  const KIRQL Old = KeAcquireInterruptSpinLock(Extension->Interrupt);
  Check(Old == Expected, 12);
  CheckIRQL(Extension->IRQL, 13);
  ++Extension->Manual;
  KeReleaseInterruptSpinLock(Extension->Interrupt,
                             Mode == 'K' ? (KIRQL)(Old + 1) : Old);
  CheckIRQL(Expected, 14);
}

static void Dpc(PKDPC Object, PVOID Context, PVOID Argument1, PVOID Argument2) {
  INTERRUPT_EXTENSION *Extension = Context;
  Check(Object == &Extension->Dpc && Argument1 == Extension &&
            Argument2 == Extension->Interrupt,
        20);
  CheckIRQL(DISPATCH_LEVEL, 21);
  ++Extension->DPCs;
  if (Mode == 'C') {
    Extension->SyncAction = 1;
    Check(!KeSynchronizeExecution(Extension->Interrupt, Critical, Extension),
          22);
    CheckIRQL(DISPATCH_LEVEL, 23);
    ManualLock(Extension, DISPATCH_LEVEL);
  }
  Extension->SyncAction = 3;
  Check(KeSynchronizeExecution(Extension->Interrupt, Critical, Extension), 24);
  CheckIRQL(DISPATCH_LEVEL, 25);
  PIRP Irp = Extension->Transfer;
  Check(Irp != NULL && Extension->Pending == NULL, 26);
  Snapshot(Extension, Irp->AssociatedIrp.SystemBuffer);
  DbgPrint("WDM interrupts: DPC unit=%lu count=%lu\n", Extension->Unit,
           Extension->DPCs);
  Complete(Irp, STATUS_SUCCESS, 32);
  Extension->Transfer = NULL;
}

static BOOLEAN Isr(PKINTERRUPT Interrupt, PVOID Context) {
  INTERRUPT_EXTENSION *Extension = Context;
  Check(Interrupt == Extension->Interrupt && Extension->Started, 30);
  CheckIRQL(Extension->IRQL, 31);
  ++Extension->ISRs;
  Extension->LastIRQL = KeGetCurrentIrql();
  DbgPrint("WDM interrupts: ISR unit=%lu count=%lu irql=%lu\n", Extension->Unit,
           Extension->ISRs, Extension->LastIRQL);
  if (Mode == 'B')
    KeWaitForSingleObject(&Extension->Event, Executive, KernelMode, FALSE,
                          NULL);
  if (!Extension->Pending)
    return FALSE;
  if (Extension->Register)
    WRITE_REGISTER_ULONG(Extension->Register,
                         READ_REGISTER_ULONG(Extension->Register) + 1);
  Check(KeInsertQueueDpc(&Extension->Dpc, Extension, Interrupt), 32);
  return TRUE;
}

static BOOLEAN MessageIsr(PKINTERRUPT Interrupt, PVOID Context,
                          ULONG MessageID) {
  UNREFERENCED_PARAMETER(Interrupt);
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(MessageID);
  Check(FALSE, 35);
  return FALSE;
}

// This original wrapper deliberately leaves nonzero undefined high return bits.
// Only AL is the BOOLEAN result; the model must not inspect the whole RAX.
__attribute__((noinline, used)) static BOOLEAN
FalseISRBody(PKINTERRUPT Interrupt, PVOID Context) {
  INTERRUPT_EXTENSION *Extension = Context;
  Check(Extension->Pending == NULL, 33);
  Check(!Isr(Interrupt, Context), 34);
  DbgPrint("WDM interrupts: unclaimed unit=%lu\n", Extension->Unit);
  return FALSE;
}

__attribute__((naked)) static BOOLEAN FalseIsr(PKINTERRUPT Interrupt
                                               __attribute__((unused)),
                                               PVOID Context
                                               __attribute__((unused))) {
  __asm__ volatile("subq $40, %rsp\n\t"
                   "callq FalseISRBody\n\t"
                   "addq $40, %rsp\n\t"
                   "movabsq $0x1122334455667700, %rax\n\t"
                   "retq");
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  INTERRUPT_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && Irp->IoStatus.Information == 0, 40);
  CheckIRQL(PASSIVE_LEVEL, 41);
  KeSetEvent(&Extension->Event, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardWait(INTERRUPT_EXTENSION *Extension, PIRP Irp) {
  KeClearEvent(&Extension->Event);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Completion, Extension, TRUE, TRUE, TRUE);
  NTSTATUS Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(KeWaitForSingleObject(&Extension->Event, Executive, KernelMode, FALSE,
                                NULL) == STATUS_SUCCESS,
          42);
  return Irp->IoStatus.Status;
}

static void Disconnect(INTERRUPT_EXTENSION *Extension) {
  if (Extension->Interrupt) {
    if (Extension->ConnectVersion) {
      IO_DISCONNECT_INTERRUPT_PARAMETERS Parameters = {0};
      Parameters.Version = Extension->ConnectVersion;
      Parameters.ConnectionContext.InterruptObject = Extension->Interrupt;
      IoDisconnectInterruptEx(&Parameters);
    } else {
      IoDisconnectInterrupt(Extension->Interrupt);
    }
    Extension->Interrupt = NULL;
    DbgPrint("WDM interrupts: disconnected unit=%lu\n", Extension->Unit);
  }
}

static void Stop(INTERRUPT_EXTENSION *Extension) {
  Check(Extension->Pending == NULL, 43);
  if (Mode != 'N')
    Disconnect(Extension);
  if (Extension->Register) {
    MmUnmapIoSpace((PVOID)Extension->Register, Extension->MapLength);
    Extension->Register = NULL;
  }
  Extension->Started = FALSE;
}

static NTSTATUS Start(INTERRUPT_EXTENSION *Extension, PCM_RESOURCE_LIST Raw,
                      PCM_RESOURCE_LIST Translated) {
  if (!Check(Raw && Translated && Raw != Translated && Raw->Count == 1 &&
                 Translated->Count == 1,
             50))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  PCM_PARTIAL_RESOURCE_LIST R = &Raw->List[0].PartialResourceList;
  PCM_PARTIAL_RESOURCE_LIST T = &Translated->List[0].PartialResourceList;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt = NULL;
  Check(R->Count == T->Count && T->Version == 1 && T->Revision == 1, 51);
  for (ULONG I = 0; I < T->Count; ++I) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor = &T->PartialDescriptors[I];
    Check(Descriptor->Type == R->PartialDescriptors[I].Type, 52);
    if (Descriptor->Type == CmResourceTypeMemory) {
      Check(Extension->Register == NULL && Descriptor->u.Memory.Length >= 4,
            53);
      Extension->MapLength = 4;
      Extension->Register =
          MmMapIoSpace(Descriptor->u.Memory.Start, 4, MmNonCached);
      Check(Extension->Register != NULL, 54);
    } else if (Descriptor->Type == CmResourceTypeInterrupt) {
      Check(Interrupt == NULL &&
                Descriptor->ShareDisposition ==
                    (Mode == 'R' ? CmResourceShareShared
                                 : CmResourceShareDeviceExclusive) &&
                Descriptor->Flags ==
                    (Mode == 'D' ? CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE
                                 : CM_RESOURCE_INTERRUPT_LATCHED) &&
                Descriptor->u.Interrupt.Affinity == 1 &&
                Descriptor->u.Interrupt.Vector !=
                    R->PartialDescriptors[I].u.Interrupt.Vector,
            55);
      Interrupt = Descriptor;
    } else {
      Check(FALSE, 56);
    }
  }
  if (!Check(Interrupt != NULL, 57))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  Extension->Vector = Interrupt->u.Interrupt.Vector;
  Extension->IRQL = (KIRQL)Interrupt->u.Interrupt.Level;
  Check(Extension->IRQL >= 3 && Extension->IRQL <= 12, 58);
  PKSERVICE_ROUTINE Service = Mode == 'F' ? FalseIsr : Isr;
  NTSTATUS Status;
  if (Mode == 'E' || Mode == 'G' || Mode == 'L' || Mode == 'Z' || Mode == 'M' ||
      Mode == 'P' || Mode == 'R' || Mode == 'D') {
    IO_CONNECT_INTERRUPT_PARAMETERS Parameters = {0};
    if (Mode == 'M' || Mode == 'P') {
      Parameters.Version =
          Mode == 'M' ? CONNECT_MESSAGE_BASED : CONNECT_MESSAGE_BASED_PASSIVE;
      Parameters.MessageBased.PhysicalDeviceObject = Extension->PDO;
      Parameters.MessageBased.ConnectionContext.InterruptObject =
          &Extension->Interrupt;
      Parameters.MessageBased.MessageServiceRoutine = MessageIsr;
      Parameters.MessageBased.ServiceContext = Extension;
      Parameters.MessageBased.FallBackServiceRoutine = Service;
    } else if (Mode == 'L') {
      Parameters.Version = CONNECT_LINE_BASED;
      Parameters.LineBased.PhysicalDeviceObject = Extension->PDO;
      Parameters.LineBased.InterruptObject = &Extension->Interrupt;
      Parameters.LineBased.ServiceRoutine = Service;
      Parameters.LineBased.ServiceContext = Extension;
      Parameters.LineBased.SynchronizeIrql = PASSIVE_LEVEL;
    } else {
      Parameters.Version = Mode == 'G' || Mode == 'Z'
                               ? CONNECT_FULLY_SPECIFIED_GROUP
                               : CONNECT_FULLY_SPECIFIED;
      Parameters.FullySpecified.PhysicalDeviceObject = Extension->PDO;
      Parameters.FullySpecified.InterruptObject = &Extension->Interrupt;
      Parameters.FullySpecified.ServiceRoutine = Service;
      Parameters.FullySpecified.ServiceContext = Extension;
      Parameters.FullySpecified.SpinLock =
          Mode == 'R' ? &SharedInterruptLock : NULL;
      Parameters.FullySpecified.ShareVector =
          Interrupt->ShareDisposition == CmResourceShareShared;
      Parameters.FullySpecified.SynchronizeIrql = Extension->IRQL;
      Parameters.FullySpecified.Vector = Extension->Vector;
      Parameters.FullySpecified.Irql = Extension->IRQL;
      Parameters.FullySpecified.InterruptMode =
          Mode == 'D' ? LevelSensitive : Latched;
      Parameters.FullySpecified.ProcessorEnableMask = 1;
      Parameters.FullySpecified.Group = Mode == 'Z' ? 1 : 0;
    }
    Status = IoConnectInterruptEx(&Parameters);
    Extension->ConnectVersion = Parameters.Version;
  } else {
    Status = IoConnectInterrupt(
        &Extension->Interrupt, Service, Extension, NULL,
        Mode == 'Q' ? Extension->Vector + 1 : Extension->Vector,
        Mode == 'J' ? (KIRQL)(Extension->IRQL + 1) : Extension->IRQL,
        Extension->IRQL, Latched, Mode == 'V', Mode == 'A' ? 2 : 1,
        Mode == 'Y');
  }
  CheckIRQL(PASSIVE_LEVEL, 59);
  if (!NT_SUCCESS(Status)) {
    Stop(Extension);
    return Status;
  }
  Check(Extension->Interrupt != NULL, 60);
  ++Extension->Starts;
  Extension->Started = TRUE;
  DbgPrint("WDM interrupts: connected unit=%lu start=%lu\n", Extension->Unit,
           Extension->Starts);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  INTERRUPT_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  const UCHAR Minor = Stack->MinorFunction;
  Check(Stack->FileObject == NULL, 70);
  CheckIRQL(PASSIVE_LEVEL, 71);
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    Stop(Extension);
    IoSkipCurrentIrpStackLocation(Irp);
    NTSTATUS Status = IoCallDriver(Lower, Irp);
    IoDetachDevice(Lower);
    --Live;
    IoDeleteDevice(Device);
    return Status;
  }
  if (Minor == IRP_MN_STOP_DEVICE || Minor == IRP_MN_SURPRISE_REMOVAL) {
    if (Minor == IRP_MN_SURPRISE_REMOVAL)
      Extension->Surprise = TRUE;
    Stop(Extension);
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Extension->Lower, Irp);
  }
  NTSTATUS Status = ForwardWait(Extension, Irp);
  if (Minor == IRP_MN_START_DEVICE && NT_SUCCESS(Status))
    Status = Start(Extension, Stack->Parameters.StartDevice.AllocatedResources,
                   Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
  else if (Minor == IRP_MN_QUERY_REMOVE_DEVICE)
    Extension->RemovePending = NT_SUCCESS(Status);
  else if (Minor == IRP_MN_CANCEL_REMOVE_DEVICE)
    Extension->RemovePending = FALSE;
  return Complete(Irp, Status, 0);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Device, PIRP Irp) {
  INTERRUPT_EXTENSION *Extension = Device->DeviceExtension;
  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(Extension->Lower, Irp);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  INTERRUPT_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  const BOOLEAN Releasing = Stack->MajorFunction == IRP_MJ_CLEANUP ||
                            Stack->MajorFunction == IRP_MJ_CLOSE;
  Check(Stack->FileObject && Stack->FileObject->DeviceObject == Extension->PDO,
        80);
  CheckIRQL(PASSIVE_LEVEL, 81);
  if (Releasing)
    return Complete(Irp, STATUS_SUCCESS, 0);
  if (Extension->Surprise)
    return Complete(Irp, STATUS_NO_SUCH_DEVICE, 0);
  if (!Extension->Started)
    return Complete(Irp, STATUS_DEVICE_NOT_READY, 0);
  if (Stack->MajorFunction == IRP_MJ_CREATE)
    return Complete(
        Irp, Extension->RemovePending ? STATUS_DELETE_PENDING : STATUS_SUCCESS,
        0);
  if (Stack->Parameters.DeviceIoControl.IoControlCode != 0x222000 ||
      Stack->Parameters.DeviceIoControl.OutputBufferLength < 32)
    return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
  if (Mode == 'T') {
    PKINTERRUPT Stale = Extension->Interrupt;
    Disconnect(Extension);
    KeSynchronizeExecution(Stale, Critical, Extension);
  }
  if (Mode == 'O')
    Check(*(volatile UCHAR *)Extension->Interrupt == 0, 82);
  Extension->Transfer = Irp;
  Extension->SyncAction = Mode == 'F' ? 0 : 2;
  Check(KeSynchronizeExecution(Extension->Interrupt, Critical, Extension), 83);
  CheckIRQL(PASSIVE_LEVEL, 84);
  Extension->SyncAction = 1;
  Check(!KeSynchronizeExecution(Extension->Interrupt, Critical, Extension), 85);
  CheckIRQL(PASSIVE_LEVEL, 86);
  ManualLock(Extension, PASSIVE_LEVEL);
  if (Mode == 'F') {
    Snapshot(Extension, Irp->AssociatedIrp.SystemBuffer);
    DbgPrint("WDM interrupts: foreground complete unit=%lu\n", Extension->Unit);
    return Complete(Irp, STATUS_SUCCESS, 32);
  }
  IoMarkIrpPending(Irp);
  DbgPrint("WDM interrupts: pending unit=%lu\n", Extension->Unit);
  return STATUS_PENDING;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status = IoCreateDevice(Driver, sizeof(INTERRUPT_EXTENSION), NULL,
                                   FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  INTERRUPT_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Unit = ++Units;
  KeInitializeEvent(&Extension->Event, NotificationEvent, FALSE);
  KeInitializeDpc(&Extension->Dpc, Dpc, Extension);
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++Live;
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(Live == 0 && Driver->DeviceObject == NULL, 90);
  DbgPrint("WDM interrupts: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  Mode = 'S';
  if (Path->Length >= sizeof(WCHAR)) {
    WCHAR Last = Path->Buffer[Path->Length / sizeof(WCHAR) - 1];
    if (Last == 'S' || Last == 'F' || Last == 'C' || Last == 'E' ||
        Last == 'G' || Last == 'L' || Last == 'Q' || Last == 'J' ||
        Last == 'A' || Last == 'N' || Last == 'T' || Last == 'B' ||
        Last == 'H' || Last == 'K' || Last == 'O' || Last == 'Z' ||
        Last == 'M' || Last == 'P' || Last == 'V' || Last == 'Y' ||
        Last == 'R' || Last == 'D')
      Mode = (UCHAR)Last;
  }
  if (Mode == 'R')
    KeInitializeSpinLock(&SharedInterruptLock);
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  DbgPrint("WDM interrupts: entry\n");
  return STATUS_SUCCESS;
}
