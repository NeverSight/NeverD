//===- driver_wdm_dma.c - Genuine WDK common and packet DMA fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bus-master driver using genuine WDK adapter function pointers.
/// Independent bus transactions change RAM; a real ISR/DPC completes the IRP.
//===----------------------------------------------------------------------===//
#include <intrin.h>
#include <ntddk.h>

#define OFFSET(Type, Field, Value)                                             \
  _Static_assert(__builtin_offsetof(Type, Field) == Value, #Type "." #Field)
_Static_assert(sizeof(DMA_ADAPTER) == 16 && _Alignof(DMA_ADAPTER) == 8 &&
                   sizeof(SCATTER_GATHER_LIST) == 16 &&
                   sizeof(SCATTER_GATHER_ELEMENT) == 24 &&
                   sizeof(PHYSICAL_ADDRESS) == 8 && sizeof(BOOLEAN) == 1,
               "genuine x64 DMA ABI");
OFFSET(DMA_ADAPTER, Version, 0);
OFFSET(DMA_ADAPTER, Size, 2);
OFFSET(DMA_ADAPTER, DmaOperations, 8);
OFFSET(DEVICE_DESCRIPTION, Version, 0);
OFFSET(DEVICE_DESCRIPTION, Master, 4);
OFFSET(DEVICE_DESCRIPTION, ScatterGather, 5);
OFFSET(DEVICE_DESCRIPTION, Dma32BitAddresses, 8);
OFFSET(DEVICE_DESCRIPTION, Dma64BitAddresses, 11);
OFFSET(DEVICE_DESCRIPTION, MaximumLength, 32);
OFFSET(DEVICE_DESCRIPTION, DmaPort, 36);
OFFSET(DMA_OPERATIONS, PutDmaAdapter, 8);
OFFSET(DMA_OPERATIONS, AllocateCommonBuffer, 16);
OFFSET(DMA_OPERATIONS, FreeCommonBuffer, 24);
OFFSET(DMA_OPERATIONS, GetDmaAlignment, 72);
OFFSET(DMA_OPERATIONS, GetScatterGatherList, 88);
OFFSET(DMA_OPERATIONS, PutScatterGatherList, 96);
OFFSET(SCATTER_GATHER_LIST, Elements, 16);
OFFSET(SCATTER_GATHER_ELEMENT, Address, 0);
OFFSET(SCATTER_GATHER_ELEMENT, Length, 8);
OFFSET(SCATTER_GATHER_ELEMENT, Reserved, 16);
_Static_assert(DEVICE_DESCRIPTION_VERSION1 == 1 &&
                   sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 20 &&
                   _Alignof(CM_PARTIAL_RESOURCE_DESCRIPTOR) == 4,
               "genuine description version and packed resources");

#define DMA_BYTES 16u
#define DMA_POOL_TAG 0x614D444Eu
#define DMA_IOCTL 0x222000u

typedef struct DMA_EXTENSION DMA_EXTENSION;
typedef struct {
  DMA_EXTENSION *Extension;
  PUCHAR Buffer;
  PMDL Mdl;
  PSCATTER_GATHER_LIST List;
  PHYSICAL_ADDRESS Logical;
  ULONG Index;
  BOOLEAN WriteToDevice;
} PACKET;

struct DMA_EXTENSION {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  PDMA_ADAPTER Adapter;
  ULONG MapRegisters;
  PUCHAR Common;
  PHYSICAL_ADDRESS CommonLogical;
  PACKET Packets[2];
  KEVENT Event;
  KDPC Dpc;
  PKINTERRUPT Interrupt;
  PIRP Pending;
  ULONG Unit;
  ULONG Starts;
  ULONG Callbacks;
  ULONG ISRs;
  ULONG DPCs;
  ULONG Ready;
  KIRQL IRQL;
  BOOLEAN Started;
  BOOLEAN Surprise;
  BOOLEAN RemovePending;
};

static UCHAR Mode;
static ULONG Units;
static ULONG Live;

static BOOLEAN Check(BOOLEAN Condition, ULONG Code) {
  if (!Condition)
    DbgPrint("WDM DMA: failure %lu\n", Code);
  return Condition;
}

static void CheckIRQL(KIRQL Expected, ULONG Code) {
  ULONG64 Raw;
  __asm__ volatile("movq %%cr8, %0" : "=r"(Raw));
  Check(KeGetCurrentIrql() == Expected && (KIRQL)Raw == Expected, Code);
}

static BOOLEAN CommonMode(void) { return Mode == 'C' || Mode == 'N'; }

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static void CompleteTransfer(DMA_EXTENSION *Extension, const UCHAR *Data) {
  PIRP Irp = Extension->Pending;
  Check(Irp != NULL, 10);
  PULONG Values = Irp->AssociatedIrp.SystemBuffer;
  Values[0] = Extension->Unit;
  Values[1] = Extension->Starts;
  Values[2] = Extension->Callbacks;
  Values[3] = Extension->ISRs;
  for (ULONG I = 0; I < DMA_BYTES; ++I)
    ((PUCHAR)Values)[16 + I] = Data[I];
  Extension->Pending = NULL;
  Extension->Ready = 0;
  Complete(Irp, STATUS_SUCCESS, 16 + DMA_BYTES);
  DbgPrint("WDM DMA: completed unit=%lu\n", Extension->Unit);
}

static void PutPacket(PACKET *Packet) {
  DMA_EXTENSION *Extension = Packet->Extension;
  PSCATTER_GATHER_LIST List = Packet->List;
  Check(List != NULL, 11);
  Packet->List = NULL;
  Extension->Adapter->DmaOperations->PutScatterGatherList(
      Extension->Adapter, List,
      Mode == 'D' ? !Packet->WriteToDevice : Packet->WriteToDevice);
  // Nothing in the released list is read after this ownership transition.
  DbgPrint("WDM DMA: put unit=%lu packet=%lu\n", Extension->Unit,
           Packet->Index);
}

static void ListControl(PDEVICE_OBJECT Device, PIRP Irp,
                        PSCATTER_GATHER_LIST List, PVOID Context) {
  PACKET *Packet = Context;
  DMA_EXTENSION *Extension = Packet->Extension;
  CheckIRQL(DISPATCH_LEVEL, 20);
  Check(Device == Extension->Self && Irp == NULL && Packet->List == NULL &&
            List != NULL && List->NumberOfElements == 1 &&
            List->Elements[0].Length == DMA_BYTES,
        21);
  Packet->List = List;
  Packet->Logical = List->Elements[0].Address;
  ++Extension->Callbacks;
  if (Mode != 'Q' || Packet->Index == 1)
    Extension->Ready = 1;
  DbgPrint("WDM DMA: callback unit=%lu packet=%lu\n", Extension->Unit,
           Packet->Index);
  if (Mode == 'U')
    Check(*(volatile UCHAR *)Packet->Buffer == 0, 22);
  if (Mode == 'F')
    IoFreeMdl(Packet->Mdl);
  if (Mode == 'G')
    ExFreePoolWithTag(Packet->Buffer, DMA_POOL_TAG);
  if (Mode == 'P')
    Extension->Adapter->DmaOperations->PutDmaAdapter(Extension->Adapter);
  if (Mode == 'I') {
    PutPacket(Packet);
    PDMA_ADAPTER Adapter = Extension->Adapter;
    Extension->Adapter = NULL;
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
    CompleteTransfer(Extension, Packet->Buffer);
    DbgPrint("WDM DMA: callback completed before return unit=%lu\n",
             Extension->Unit);
  }
  // Deliberately return void: arbitrary RAX is not an NTSTATUS completion.
}
_Static_assert(__builtin_types_compatible_p(__typeof__(&ListControl),
                                            PDRIVER_LIST_CONTROL),
               "genuine four-argument void adapter list callback");

static void GetPacket(PACKET *Packet) {
  DMA_EXTENSION *Extension = Packet->Extension;
  NTSTATUS Status = Extension->Adapter->DmaOperations->GetScatterGatherList(
      Extension->Adapter, Extension->Self, Packet->Mdl, Packet->Buffer,
      DMA_BYTES, ListControl, Packet, Packet->WriteToDevice);
  Check(Status == STATUS_SUCCESS, 23);
  DbgPrint("WDM DMA: get returned unit=%lu packet=%lu\n", Extension->Unit,
           Packet->Index);
}

static void Dpc(PKDPC Object, PVOID Context, PVOID Argument1, PVOID Argument2) {
  DMA_EXTENSION *Extension = Context;
  CheckIRQL(DISPATCH_LEVEL, 30);
  Check(Object == &Extension->Dpc && Argument2 == Extension, 31);
  ++Extension->DPCs;
  if ((ULONG_PTR)Argument1 == 1) {
    GetPacket(&Extension->Packets[0]);
    if (Mode == 'Q') {
      Check(Extension->Callbacks == 1 && Extension->MapRegisters == 1, 32);
      GetPacket(&Extension->Packets[1]);
      Check(Extension->Callbacks == 1 &&
                Extension->Packets[1].List == NULL,
            33);
      DbgPrint("WDM DMA: second packet queued unit=%lu\n", Extension->Unit);
      PutPacket(&Extension->Packets[0]);
    }
    return;
  }
  Check((ULONG_PTR)Argument1 == 2 && Extension->Ready, 34);
  if (CommonMode()) {
    CompleteTransfer(Extension, Extension->Common);
  } else {
    PACKET *Packet = &Extension->Packets[Mode == 'Q' ? 1 : 0];
    PutPacket(Packet);
    // Put flushes packet DMA ownership before the CPU inspects transferred RAM.
    CompleteTransfer(Extension, Packet->Buffer);
  }
  DbgPrint("WDM DMA: interrupt DPC returned unit=%lu\n", Extension->Unit);
}

static BOOLEAN Isr(PKINTERRUPT Interrupt, PVOID Context) {
  DMA_EXTENSION *Extension = Context;
  CheckIRQL(Extension->IRQL, 40);
  Check(Interrupt == Extension->Interrupt && Extension->Started, 41);
  ++Extension->ISRs;
  if (!Extension->Pending || !Extension->Ready)
    return FALSE;
  Check(KeInsertQueueDpc(&Extension->Dpc, (PVOID)2, Extension), 42);
  DbgPrint("WDM DMA: ISR unit=%lu\n", Extension->Unit);
  return TRUE;
}

static NTSTATUS Completion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  DMA_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && Irp->IoStatus.Information == 0, 50);
  CheckIRQL(PASSIVE_LEVEL, 51);
  KeSetEvent(&Extension->Event, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardWait(DMA_EXTENSION *Extension, PIRP Irp) {
  KeClearEvent(&Extension->Event);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, Completion, Extension, TRUE, TRUE, TRUE);
  NTSTATUS Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(KeWaitForSingleObject(&Extension->Event, Executive, KernelMode, FALSE,
                                NULL) == STATUS_SUCCESS,
          52);
  return Irp->IoStatus.Status;
}

static NTSTATUS AcquireAdapter(DMA_EXTENSION *Extension) {
  DEVICE_DESCRIPTION Description = {0};
  Description.Version = DEVICE_DESCRIPTION_VERSION1;
  Description.Master = TRUE;
  Description.ScatterGather = TRUE;
  Description.Dma32BitAddresses = TRUE;
  Description.Dma64BitAddresses = TRUE;
  Description.InterfaceType = Internal;
  Description.MaximumLength = PAGE_SIZE;
  // These genuine bus-master fields have no subordinate-DMA meaning.
  Description.DemandMode = TRUE;
  Description.AutoInitialize = TRUE;
  Description.BusNumber = 37;
  Description.DmaChannel = 7;
  Description.DmaWidth = Width8Bits;
  Description.DmaSpeed = Compatible;
  Description.DmaPort = 9;
  CheckIRQL(PASSIVE_LEVEL, 60);
  Extension->Adapter = IoGetDmaAdapter(Extension->PDO, &Description,
                                      &Extension->MapRegisters);
  if (!Extension->Adapter)
    return STATUS_INSUFFICIENT_RESOURCES;
  PDMA_ADAPTER Adapter = Extension->Adapter;
  Check(Adapter->Version == 1 && Adapter->Size >= sizeof(DMA_ADAPTER) &&
            Adapter->DmaOperations->Size >=
                __builtin_offsetof(DMA_OPERATIONS, PutScatterGatherList) +
                    sizeof(PVOID) &&
            Adapter->DmaOperations->GetDmaAlignment(Adapter) == 1 &&
            Extension->MapRegisters != 0,
        61);
  DbgPrint("WDM DMA: adapter unit=%lu registers=%lu\n", Extension->Unit,
           Extension->MapRegisters);
  return STATUS_SUCCESS;
}

static void Stop(DMA_EXTENSION *Extension) {
  CheckIRQL(PASSIVE_LEVEL, 70);
  Check(Extension->Pending == NULL, 71);
  if (Extension->Interrupt) {
    IoDisconnectInterrupt(Extension->Interrupt);
    Extension->Interrupt = NULL;
  }
  if (Mode == 'N')
    return; // Deliberate ownership violation, checked at STOP/REMOVE completion.
  if (Extension->Common) {
    Extension->Adapter->DmaOperations->FreeCommonBuffer(
        Extension->Adapter, DMA_BYTES, Extension->CommonLogical,
        Extension->Common, TRUE);
    Extension->Common = NULL;
  }
  for (ULONG I = 0; I < 2; ++I) {
    Check(Extension->Packets[I].List == NULL, 72);
    if (Extension->Packets[I].Mdl) {
      IoFreeMdl(Extension->Packets[I].Mdl);
      Extension->Packets[I].Mdl = NULL;
    }
    if (Extension->Packets[I].Buffer) {
      ExFreePoolWithTag(Extension->Packets[I].Buffer, DMA_POOL_TAG);
      Extension->Packets[I].Buffer = NULL;
    }
  }
  if (Extension->Adapter) {
    PDMA_ADAPTER Adapter = Extension->Adapter;
    Extension->Adapter = NULL;
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
  }
  Extension->Started = FALSE;
  DbgPrint("WDM DMA: stopped unit=%lu\n", Extension->Unit);
}

static NTSTATUS Start(DMA_EXTENSION *Extension, PCM_RESOURCE_LIST Raw,
                      PCM_RESOURCE_LIST Translated) {
  Check(Raw != NULL && Translated != NULL && Raw != Translated &&
            Raw->Count == 1 && Translated->Count == 1,
        80);
  PCM_PARTIAL_RESOURCE_LIST Resources = &Translated->List[0].PartialResourceList;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt = NULL;
  for (ULONG I = 0; I < Resources->Count; ++I)
    if (Resources->PartialDescriptors[I].Type == CmResourceTypeInterrupt)
      Interrupt = &Resources->PartialDescriptors[I];
  if (!Check(Interrupt != NULL &&
                 Interrupt->ShareDisposition == CmResourceShareDeviceExclusive &&
                 Interrupt->Flags == CM_RESOURCE_INTERRUPT_LATCHED &&
                 Interrupt->u.Interrupt.Affinity == 1,
             81))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  NTSTATUS Status = STATUS_SUCCESS;
  if (!Extension->Adapter)
    Status = AcquireAdapter(Extension);
  if (!NT_SUCCESS(Status))
    return Status;
  if (CommonMode()) {
    Extension->Common = Extension->Adapter->DmaOperations->AllocateCommonBuffer(
        Extension->Adapter, DMA_BYTES, &Extension->CommonLogical, TRUE);
    if (!Extension->Common)
      return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG I = 0; I < DMA_BYTES; ++I)
      Extension->Common[I] = (UCHAR)(0x10 + Extension->Unit + I);
  } else {
    for (ULONG I = 0; I < (Mode == 'Q' ? 2u : 1u); ++I) {
      PACKET *Packet = &Extension->Packets[I];
      Packet->Extension = Extension;
      Packet->Index = I;
      Packet->WriteToDevice = Mode != 'R' && Mode != 'Q';
      Packet->Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE,
                                             DMA_POOL_TAG);
      if (!Packet->Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
      Check(((ULONG_PTR)Packet->Buffer & (PAGE_SIZE - 1)) == 0, 82);
      for (ULONG J = 0; J < DMA_BYTES; ++J)
        Packet->Buffer[J] = (UCHAR)(0x20 + Extension->Unit + I * 0x20 + J);
      Packet->Mdl = IoAllocateMdl(Packet->Buffer, DMA_BYTES, FALSE, FALSE, NULL);
      if (!Packet->Mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
      MmBuildMdlForNonPagedPool(Packet->Mdl);
    }
  }
  Extension->IRQL = (KIRQL)Interrupt->u.Interrupt.Level;
  Status = IoConnectInterrupt(
      &Extension->Interrupt, Isr, Extension, NULL, Interrupt->u.Interrupt.Vector,
      Extension->IRQL, Extension->IRQL, Latched, FALSE, 1, FALSE);
  if (!NT_SUCCESS(Status))
    return Status;
  ++Extension->Starts;
  Extension->Started = TRUE;
  DbgPrint("WDM DMA: started unit=%lu count=%lu\n", Extension->Unit,
           Extension->Starts);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  DMA_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  UCHAR Minor = Stack->MinorFunction;
  CheckIRQL(PASSIVE_LEVEL, 90);
  Check(Stack->FileObject == NULL, 91);
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
  if (Minor == IRP_MN_START_DEVICE) {
    if (NT_SUCCESS(Status))
      Status = Start(Extension, Stack->Parameters.StartDevice.AllocatedResources,
                     Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
    if (!NT_SUCCESS(Status))
      Stop(Extension);
  } else if (Minor == IRP_MN_QUERY_REMOVE_DEVICE) {
    Extension->RemovePending = NT_SUCCESS(Status);
  } else if (Minor == IRP_MN_CANCEL_REMOVE_DEVICE) {
    Extension->RemovePending = FALSE;
  }
  return Complete(Irp, Status, 0);
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Device, PIRP Irp) {
  DMA_EXTENSION *Extension = Device->DeviceExtension;
  PoStartNextPowerIrp(Irp);
  IoSkipCurrentIrpStackLocation(Irp);
  return PoCallDriver(Extension->Lower, Irp);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  DMA_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  CheckIRQL(PASSIVE_LEVEL, 100);
  Check(Stack->FileObject && Stack->FileObject->DeviceObject == Extension->PDO,
        101);
  if (Stack->MajorFunction == IRP_MJ_CLEANUP ||
      Stack->MajorFunction == IRP_MJ_CLOSE)
    return Complete(Irp, STATUS_SUCCESS, 0);
  if (Extension->Surprise)
    return Complete(Irp, STATUS_NO_SUCH_DEVICE, 0);
  if (!Extension->Started)
    return Complete(Irp, STATUS_DEVICE_NOT_READY, 0);
  if (Stack->MajorFunction == IRP_MJ_CREATE)
    return Complete(Irp, Extension->RemovePending ? STATUS_DELETE_PENDING
                                                 : STATUS_SUCCESS,
                    0);
  if (Stack->Parameters.DeviceIoControl.IoControlCode !=
          (Mode == 'V' ? DMA_IOCTL + METHOD_IN_DIRECT : DMA_IOCTL) ||
      Stack->Parameters.DeviceIoControl.OutputBufferLength < 32)
    return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
  Check(Extension->Pending == NULL, 102);
  if (Mode == 'T')
    Extension->Adapter->DmaOperations->ReadDmaCounter(Extension->Adapter);
  if (Mode == 'V') {
    // This request-owned METHOD_IN_DIRECT MDL was locked for device reads.
    // Asking the adapter to write through it must reject before any callback.
    Extension->Packets[0].Mdl = Irp->MdlAddress;
    Extension->Packets[0].Buffer = MmGetMdlVirtualAddress(Irp->MdlAddress);
    Extension->Packets[0].WriteToDevice = FALSE;
  }
  Extension->Pending = Irp;
  IoMarkIrpPending(Irp);
  if (CommonMode()) {
    Extension->Ready = 1;
  } else {
    Check(KeInsertQueueDpc(&Extension->Dpc, (PVOID)1, Extension), 103);
  }
  DbgPrint("WDM DMA: pending unit=%lu\n", Extension->Unit);
  return STATUS_PENDING;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status = IoCreateDevice(Driver, sizeof(DMA_EXTENSION), NULL,
                                   FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                   FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  DMA_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Unit = ++Units;
  KeInitializeEvent(&Extension->Event, NotificationEvent, FALSE);
  KeInitializeDpc(&Extension->Dpc, Dpc, Extension);
  // IoGetDmaAdapter is intentionally before START and before attachment.
  Status = AcquireAdapter(Extension);
  if (!NT_SUCCESS(Status)) {
    IoDeleteDevice(Device);
    return Status;
  }
  if (Mode == 'A' || Mode == 'B') {
    if (Mode == 'B')
      Extension->Adapter->DmaOperations->PutDmaAdapter(Extension->Adapter);
    // A deliberately leaves the PDO-owned adapter alive after removing FDO.
    IoDeleteDevice(Device);
    return STATUS_UNSUCCESSFUL;
  }
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    Extension->Adapter->DmaOperations->PutDmaAdapter(Extension->Adapter);
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  ++Live;
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

static void Unload(PDRIVER_OBJECT Driver) {
  Check(Live == 0 && Driver->DeviceObject == NULL, 110);
  DbgPrint("WDM DMA: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  Mode = 'C';
  if (Path->Length >= sizeof(WCHAR)) {
    WCHAR Last = Path->Buffer[Path->Length / sizeof(WCHAR) - 1];
    if (Last == 'C' || Last == 'S' || Last == 'R' || Last == 'Q' || Last == 'I' ||
        Last == 'U' || Last == 'F' || Last == 'G' || Last == 'P' || Last == 'D' ||
        Last == 'T' || Last == 'N' || Last == 'V' || Last == 'A' ||
        Last == 'B')
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
  DbgPrint("WDM DMA: entry mode=%c\n", Mode);
  return STATUS_SUCCESS;
}
